// CircuitDrift runtime: oversampled electrical solve with thermal clock and solver-failure fallback
// (P03 integration; 01-runtime "Coupled simulation contract", "Oversampling and quality").
// Owner: cd_runtime. Depends only on cd_core/cd_dsp.
//
// Per host sample (order from the spec, restricted to what exists): upsampled input -> high-rate
// latency pad -> source volts -> MnaSolver substep x OS -> dissipative power accumulation -> due
// 1 kHz thermal update -> downsample -> solver-fallback crossfade to latency-aligned dry.
//
// SYNTHETIC CONTRACTS (not physical, G01 open):
//  * I/O mapping: source volts = input * voltsPerUnit on one VoltageSource element; output =
//    V(outputNode) * unitsPerVolt. Units are the fixture's volts; no hardware calibration.
//  * Dissipative power: current manifests carry no dissipative-term flag or electrical->thermal
//    node map, so this runtime defines one explicitly (SyntheticThermal): only elements whose
//    manifest equation family is a static branch (Resistor, ShockleyDiode) may be mapped; their
//    instantaneous v*i [W] from the solved branch voltage is averaged over the OS substeps of each
//    host sample. Capacitors (reactive storage) and sources (supplied power) are rejected. Audio
//    amplitude is never used as watts.
//  * Thermal feedback: none. MnaSolver has no parameter-update API and manifests define no
//    temperature law, so temperatures are telemetry only. When feedback is added, a new T affects
//    the sample after its tick (one-tick lag), which reference comparisons must document.
//  * Thermal clock: 1000 Hz fractional phase accumulator advanced per host sample, independent of
//    callback boundaries; the host sample straddling a tick boundary has its energy split exactly.
//    The accumulator is an exact integer (phase = acc/fs, +1000 per sample), so integer-Hz rates
//    are required (non-integer rates are rejected explicitly); floating accumulation drifted by a
//    whole tick over 10 s at 44.1 kHz.
//
// Failure policy: a failed substep reuses the last finite state (MnaSolver never commits invalid
// state) and is counted. Three consecutive failed substeps LATCH fallback (MnaSolver::needsFallback
// clears after one success, so the runtime keeps its own latch): wet fades to latency-aligned dry
// over 10 ms, the diagnostic is preserved and resetRequested() is set for an off-thread operating
// point reset. Re-entry only via reset() (restores the prepared DC state) or a new prepare().
// The prepared DC operating point is always a valid prior sample, so "no prior valid sample" does
// not arise after a successful prepare.
//
// Latency: dry is delayed by the integer declared latency; wet is padded inside the oversampled
// domain by an integer number of high-rate samples (exact for FIR; see LatencyPlan).
//
// Lifetime: prepare() deep-copies every referenced DeviceManifest; NetElement::model of the caller's
// netlist is never retained. prepare() builds a complete candidate and installs it only on success;
// on failure the previously prepared state is untouched. The object is non-copyable/non-movable so
// solver model pointers into the owned copies stay valid.
// Wiring additions (Engine integration): an optional per-sample drive gain multiplies the circuit
// input only (wet path; dry and the fallback target stay undriven), used for the Warmth macro's
// pre-stage drive. saveState()/loadState() serialise every dynamic field (solver unknowns and
// charges via MnaSolver::getState, oversampler histories, latency pads, dry delay, thermal
// temperatures/energy, 1 kHz clock accumulator, fallback latch/fade, counters) for engine
// snapshots; modelHash() identifies the prepared topology/quality for snapshot compatibility.
// Threading: prepare() off the audio thread; process()/reset()/accessors noexcept, allocation-free.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "dsp/MnaSolver.h"
#include "dsp/Oversampling.h"
#include "dsp/ThermalNetwork.h"
#include "runtime/RuntimeCommon.h"

namespace cd::runtime {

struct SyntheticIo {
    int sourceElement = 0;          // index of a VoltageSource element
    double voltsPerUnit = 1.0;      // host full-scale 1.0 -> volts (synthetic)
    int outputNode = 1;             // 1..nodes
    double unitsPerVolt = 1.0;      // volts -> host units (synthetic)
};

struct SyntheticThermal {
    bool enabled = true;
    dsp::ThermalNetwork::Config network{};  // per channel; default (n==0) -> T20 node C=2 J/K, G=0.2 W/K
    double ambientK = 298.15;
    // Per netlist element: thermal node index or -1. Empty -> every Resistor/Diode element maps to
    // node 0. Mapping a capacitor, a source or an out-of-range node is rejected.
    std::vector<int> elementToNode;
};

struct CircuitConfig {
    dsp::Netlist netlist;
    SyntheticIo io;
    SyntheticThermal thermal;
    double sampleRate = 48000;      // integer Hz, 44.1-192 kHz (tested 44.1/48/96/192 only)
    int maxBlock = 512;             // 1..8192 prepared slice
    int channels = 1;               // 1 or 2 (independent electrical state per channel)
    dsp::Oversampler::Spec os{};    // factor 1..16 normal; 32/64 only for offline reference tests
    int maxNewton = 8;              // Eco 6, Studio 8, Render 12
    bool padToFamilyMax = true;     // false for explicit Zero Latency
};

// Labelled SYNTHETIC fixtures (catalogue manifests fixture.*), not hardware models.
// Antiparallel diode clipper: Vin -R(1k)- n2, D n2->0, D 0->n2, C(10 nF) n2->0; out = V(n2).
CircuitConfig syntheticDiodeClipper();
// RC low-pass: Vin -R- n2 -C- 0; out = V(n2).
CircuitConfig syntheticRcLowpass(double resistanceOhm, double capacitanceF);
// Linear resistive divider with equal resistors; unitsPerVolt chooses the wet gain (2 -> unity).
CircuitConfig syntheticDivider(double unitsPerVolt);

class CircuitRuntime {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr double kThermalRateHz = 1000.0;
    static constexpr double kFallbackFadeSec = 0.010;
    static constexpr int kFallbackAfterConsecutive = 3;

    struct Diagnostics {
        int64_t samples = 0;            // host samples processed since prepare (not reset by reset())
        std::int64_t failedSubsteps = 0;
        std::int64_t consecutiveFailures = 0;   // current run (max over channels)
        bool fallbackLatched = false;
        bool resetRequested = false;
        double fallbackWeight = 0;      // 0 = circuit, 1 = dry
        std::int64_t thermalTicks = 0;
        double thermalPhase = 0;        // [0,1) ticks
    };

    CircuitRuntime();
    ~CircuitRuntime();
    CircuitRuntime(const CircuitRuntime&) = delete;
    CircuitRuntime& operator=(const CircuitRuntime&) = delete;

    bool prepare(const CircuitConfig& cfg, std::string& error);
    bool prepared() const noexcept { return core_ != nullptr; }

    // n in [0, maxBlock]; in/wet/dry: channels() pointers. wet = circuit output aligned at
    // latencySamples(); dry = in delayed by latencySamples(). in may alias neither output.
    // Unprepared or out-of-range calls write nothing and advance nothing.
    // driveGain: null or n per-sample gains applied to the circuit input only.
    void process(const double* const* in, double* const* wet, double* const* dry, int n,
                 const double* driveGain = nullptr) noexcept;
    // Snapshot support (audio-safe, allocation-free). loadState(apply=false) only validates.
    void saveState(StateWriter& w) const noexcept;
    bool loadState(StateReader& r, bool apply) noexcept;
    std::size_t stateBytes() const noexcept;
    std::uint64_t modelHash() const noexcept;
    // Restores the prepared DC state, filters, delays, thermal state/clock and clears the latch.
    void reset() noexcept;

    int latencySamples() const noexcept;
    const LatencyPlan& latencyPlan() const noexcept;
    int channels() const noexcept;
    int factor() const noexcept;
    int maxBlock() const noexcept;
    double sampleRate() const noexcept;
    Diagnostics diagnostics() const noexcept;
    double temperatureK(int ch, int node) const noexcept;
    double lastPowerW(int ch, int node) const noexcept;   // host-sample average of the last sample
    double voltage(int ch, int node) const noexcept;
    int ownedManifestCount() const noexcept;
    // Telemetry (audio-safe reads of the last solved substep). Branches = the dissipative elements
    // mapped to thermal nodes (resistors/diodes); current from the same branch law used for power.
    struct BranchTelemetry {
        int element = 0, thermalNode = 0;
        double voltageV = 0, currentA = 0, powerW = 0, temperatureK = 0;
    };
    int telemetryBranches() const noexcept;
    bool branchTelemetry(int ch, int k, BranchTelemetry& out) const noexcept;
    double inputVolts(int ch) const noexcept;    // source element voltage (synthetic volts)
    double outputVolts(int ch) const noexcept;   // V(outputNode)
    int lastIterations(int ch) const noexcept;
    double lastResidual(int ch) const noexcept;

private:
    struct Core;
    std::unique_ptr<Core> core_;
};

}  // namespace cd::runtime
