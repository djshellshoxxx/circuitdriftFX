// CircuitDrift runtime: oversampled electrical solve with thermal clock and solver-failure fallback
// (P03 integration; 01-runtime "Coupled simulation contract", "Oversampling and quality").
// Owner: cd_runtime. Depends on cd_core/cd_dsp and cd_control (composeLoadCurrent only).
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
//  * Thermal feedback (resume packet 2): every Device element whose manifest declares a
//    TemperatureLaw AND that the thermal map assigns to a node is a feedback element. At each
//    1 kHz tick, after the heat-network step, its parameter becomes law(nominal, T_node, k) via
//    MnaSolver::setElementParameter (bounded, noexcept, allocation-free), with k the per-sample
//    temperature-sensitivity control (Drift: 1+4d; default 1). ONE-TICK LAG: the new T affects the
//    solver from the host sample AFTER the tick sample (the tick sample itself was already solved
//    with the old value); reference comparisons must model this lag. Power accounting uses the
//    updated parameter. Clamps of T (manifest domain) or of the value (parameter bounds) are counted.
//    Only synthetic tempco fixtures exist (fixture.resistor.tempco; G01 open).
//  * Supply reservoir (resume packet 3/T15/T16): an EXPLICIT REDUCED SUPPLY MODEL selected by
//    CircuitConfig::supply (not placed in the MNA netlist). Per channel, per oversampled substep h:
//    backward Euler C*(V-V_old)/h = I_rect(V) - I_load with an ideal rectifier diode in series with
//    the source impedance, I_rect = max(0, (V_src - V)/R_src); V is bounded to
//    [r_min*V_nom, V_src] (minimum-rail manifest bound, clamps counted). V_nom = V_src - R_src*I_idle
//    (idle equilibrium, r = 1 at rest). The circuit output volts x drive a rail-referenced stage
//    y = H*tanh(x/H), H = headroomV * r_eff, whose draw is I_phys = I_idle + I_dyn with
//    I_dyn = |x|/R_load (drive-referenced, so the reservoir trajectory does not depend on r_eff);
//    I_load = cd::control::composeLoadCurrent(source, I_phys, I_dyn, s, I_ext). C = C_eff from the
//    Sag macro (max(C_min, C_base/(1+3s))); a C change keeps V continuous. r = V/V_nom is a
//    control/readout; the device equation uses volts (H). r_eff = r + P(1-r) (TransientDetector
//    preservation P) changes only the stage headroom: the physical reservoir V is unchanged.
//    SYNTHETIC numbers (syntheticFactorySupply), not a calibrated supply (G01 open).
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

// Reduced SYNTHETIC supply model (see header). Volts/amps/farads of the synthetic fixture only.
struct SyntheticSupply {
    bool enabled = false;
    double sourceV = 1.1;           // rectified DC source V_src [V]
    double sourceOhm = 100;         // rectifier/source impedance R_src [Ohm]
    double cBaseF = 150e-6;         // reservoir C_base [F] (Sag binding)
    double cMinF = 15e-6;           // reservoir C_min [F] (Sag binding)
    double idleA = 1e-3;            // idle draw I_idle [A]
    double loadOhm = 270;           // signal draw I_dyn = |x| / R_load [A]
    double headroomV = 0.5;         // stage headroom at r_eff = 1 [V]
    double minRailFraction = 0.2;   // minimum rail bound r_min (manifest)
    double iBudgetA = 4e-3;         // external sidechain current budget I_budget [A]
    double nominalV() const noexcept { return sourceV - sourceOhm * idleA; }
};
// Synthetic factory supply numbers (round, arbitrary; chosen so Sag is audible on the fixture).
SyntheticSupply syntheticFactorySupply() noexcept;

enum class LoadSource : std::uint8_t { Main = 0, External = 1, Blend = 2 };

// Per-sample controls for process(); every pointer is null (neutral) or holds n values.
struct CircuitControls {
    const double* drive = nullptr;          // pre-stage gain on the circuit input (wet only)
    const double* reservoirC = nullptr;     // C_eff [F]; null -> C_base
    const double* sag = nullptr;            // smoothed Sag s in [0,1]; null -> 0
    const double* preserve = nullptr;       // transient preservation P in [0,1]; null -> 0
    const double* extCurrent = nullptr;     // I_ext [A] (>= 0); null -> 0
    const double* tempSensitivity = nullptr;  // temperature-law multiplier k; null -> 1
    LoadSource source = LoadSource::Main;
};

struct CircuitConfig {
    dsp::Netlist netlist;
    SyntheticIo io;
    SyntheticThermal thermal;
    SyntheticSupply supply;         // disabled by default (no supply in the original fixtures)
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
        // supply / thermal feedback
        bool supplyEnabled = false;
        double railVolts[2] = {0, 0}, railFraction[2] = {0, 0}, effectiveRailFraction[2] = {0, 0};
        double loadCurrentA[2] = {0, 0}, reservoirF = 0;
        std::int64_t railClamps = 0;
        int feedbackElements = 0;
        std::int64_t feedbackUpdates = 0, feedbackClamps = 0, feedbackRejected = 0;
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
    // Same with the full per-sample control set (supply, preservation, sidechain, temperature law).
    void process(const double* const* in, double* const* wet, double* const* dry, int n,
                 const CircuitControls& controls) noexcept;
    // Audio-safe: copies the slow physical state (thermal temperatures/energy, 1 kHz clock phase,
    // reservoir rail volts, feedback parameter values) from another prepared runtime with the same
    // channel count, thermal node count, feedback layout and supply presence (live quality
    // transition). False leaves *this unchanged.
    bool adoptSlowState(const CircuitRuntime& other) noexcept;
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
    // Current solver value of element parameter k (temperature feedback readout); NaN if invalid.
    double elementParameter(int ch, int element, int k) const noexcept;
    // Rail fraction r at the last substep of each host sample of the last process() call (n values).
    const double* railFractionTrace(int ch) const noexcept;
    const SyntheticSupply& supply() const noexcept;
    bool hasTemperatureFeedback() const noexcept;

private:
    struct Core;
    std::unique_ptr<Core> core_;
};

}  // namespace cd::runtime
