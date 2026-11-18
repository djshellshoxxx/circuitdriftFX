// CircuitDrift dsp: MnaSolver inside the Oversampler (P03 integration; 01-runtime "Coupled simulation
// contract", "Oversampling and quality", "Output protection"). Mono stage; one instance per channel.
// Wet path per host sample: up (Oversampler) -> F electrical backward-Euler substeps -> fractional-delay
// pad -> down. Dry path: integer delay of the declared latency. Validated only on labelled synthetic
// fixtures; no physical claim (G01), no T21 claim (G03).
// Timebases: everything advances one host sample at a time in a fixed order, so any block partition
// (including zero-frame blocks) is bit-identical to one large block. Thermal updates at 1 kHz from a
// fractional phase accumulator in host samples; power = dissipation of resistor/diode elements averaged
// over the tick. The thermal state is telemetry only: the current manifests carry no temperature
// dependence, so there is no thermal->electrical feedback yet (documented gap).
// Containment (spec 01-runtime): a failed substep keeps the last valid solver state (MnaSolver::step);
// 3 consecutive failed substeps, a non-finite circuit/wet sample, or |wet| > 8 starts a 10 ms linear
// fade to latency-aligned dry (|wet| is clamped to 8 meanwhile; non-finite is held). No valid sample yet
// -> delayed dry immediately. After the fade the solver is no longer stepped; re-entry only via
// reset() (restores the prepared operating point) or a fresh prepare(). Non-finite input is replaced by
// zero. Latency: declared = ceil(oversampler latency) (or a larger explicit target); the sub-sample
// remainder is padded in the oversampled domain, so FIR wet == dry delay in the passband. IIR latency is
// nominal (DC group delay): dry alignment there is approximate by construction.
// Threading: prepare() allocates and solves the DC point (off-thread). process()/reset() and all
// accessors are noexcept, lock/IO/allocation-free, bounded work, double precision state.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "dsp/MnaSolver.h"
#include "dsp/Oversampling.h"
#include "dsp/ThermalNetwork.h"

namespace cd::dsp {

enum class StageState : std::uint8_t { Running, Fading, DryFallback };
enum class FallbackReason : std::uint8_t { None, NoValidSample, SolverFailures, NonFiniteWet, Overamplitude };

class CircuitStage {
public:
    static constexpr int kMaxLatency = 4096;           // declared base-rate samples
    static constexpr double kEmergencyAmplitude = 8.0; // spec: |y| > 8 enters the dry fallback
    static constexpr double kInputClamp = 1.0e6;       // finite-input guard (keeps filter state finite)
    static constexpr double kHiRateClamp = 64.0;       // oversampled-domain guard, far above kEmergencyAmplitude
    static constexpr int kFailuresToFallback = 3;

    struct Config {
        Netlist net;
        int inputSource = 0;                // element index of the VoltageSource driven by audio
        int outputNode = 1;                 // 1..net.nodes
        double inputVoltsPerFs = 1.0;       // volts per full-scale input unit, finite > 0
        double outputVoltsPerFs = 1.0;      // volts per full-scale output unit, finite > 0 (1/x finite)
        double fs = 48000.0;                // host rate
        Oversampler::Spec os;               // product default: 4x FIR
        MnaSolver::Options newton;          // Eco 6 / Studio 8 / Render 12
        ThermalNetwork::Config thermal;     // thermal.n == 0 disables the thermal network
        std::vector<int> elementThermalNode;// per net element; empty -> all dissipation into node 0
        double ambientK = 298.15;
        int latencyTarget = 0;              // 0 = ceil(oversampler latency); else >= that, <= kMaxLatency
        double fadeSeconds = 0.010;         // spec: 10 ms
        double thermalHz = 1000.0;          // spec: 1 kHz
    };

    // Transactional: on false the previous preparation (if any) is untouched. Off the audio thread.
    bool prepare(const Config& cfg, std::string& error);
    bool prepared() const noexcept { return prepared_; }
    // Restores the prepared operating point, thermal ambient, empties all delay lines and re-enters
    // Running. Allocation-free; this is the explicit re-entry after a fallback.
    void reset() noexcept;
    // in/out may alias; n may be 0 (pure no-op, pointers may be null). Unprepared: sanitised pass-through.
    void process(const double* in, double* out, std::size_t n) noexcept;

    int latency() const noexcept { return latency_; }                       // declared, base-rate samples
    double oversamplerLatency() const noexcept { return os_.latency(); }    // raw round trip (may be fractional)
    int padHighRateSamples() const noexcept { return padLen_; }             // fractional-delay compensation
    int fadeSamples() const noexcept { return fadeN_; }

    StageState state() const noexcept { return state_; }
    FallbackReason fallbackReason() const noexcept { return reason_; }
    std::int64_t fallbackSampleIndex() const noexcept { return fallbackAt_; }  // host index, -1 if none
    long solverFailures() const noexcept { return stepFailures_; }
    long inputRepairs() const noexcept { return inputRepairs_; }
    std::uint64_t hostSamples() const noexcept { return hostSamples_; }
    std::uint64_t thermalTicks() const noexcept { return thermalTicks_; }
    double temperatureK(std::size_t node) const noexcept { return node < therm_.size() ? therm_.temperature(node) : 0.0; }

private:
    struct PowerElem { enum class K : std::uint8_t { None, Resistor, Diode } k = K::None; int a = 0, b = 0, node = 0;
                       double p0 = 0, p1 = 0, p2 = 0, cap = 40; };
    double sample(double x) noexcept;
    double dissipation(const PowerElem& e) const noexcept;
    void startFade(FallbackReason why) noexcept;

    bool prepared_ = false;
    Oversampler os_;
    MnaSolver sol_;
    ThermalNetwork therm_;
    std::vector<PowerElem> power_;
    std::vector<double> initState_, dry_, pad_, hi_;
    std::vector<double> initTempK_;
    double energy_[ThermalNetwork::kMaxNodes] = {};
    double accTime_ = 0, phase_ = 0, period_ = 48, h_ = 0, invOutV_ = 1, inV_ = 1;
    int factor_ = 1, latency_ = 0, padLen_ = 0, fadeN_ = 1, src_ = 0, outNode_ = 1, thermalN_ = 0;
    std::size_t dryPos_ = 0, padPos_ = 0;
    // dynamic state
    StageState state_ = StageState::Running;
    FallbackReason reason_ = FallbackReason::None;
    std::int64_t fallbackAt_ = -1;
    int fadePos_ = 0, failStreak_ = 0;
    bool haveValid_ = false;
    double lastWet_ = 0, lastHi_ = 0;
    long stepFailures_ = 0, inputRepairs_ = 0;
    std::uint64_t hostSamples_ = 0, thermalTicks_ = 0;
};

}  // namespace cd::dsp
