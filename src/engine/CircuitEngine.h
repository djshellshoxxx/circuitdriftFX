// CircuitDrift engine: integrated circuit runtime (P03; CD-F20/F21/F22/F23/F25).
//
// Chain (01-runtime order): input gain (10 ms) -> oversampled circuit per A/B slot (both slots
// always run; stereo, L/R tolerance variance) with reduced supply, 1 kHz thermal network, noise
// and creative stages -> downsample -> latency-aligned level-matched blend (mix 10 ms) -> output
// trim (10 ms) -> HPF (20 ms enable crossfade, bounded 20 ms cutoff smoothing) -> soft clip in its
// own oversampled stage (20 ms enable crossfade) -> emergency containment -> host bypass (10 ms).
//
// Physical models are SYNTHETIC FIXTURES (G01 open): never a calibrated hardware claim.
//
// Threading: prepare()/checkpointSize() run off the audio thread (allocate). process(), reset(),
// setBypassed(), needsReprepare(), getCheckpoint()/setCheckpoint() (into preallocated storage)
// are noexcept, bounded and allocation-free on the audio thread. readTelemetry() is the single UI
// reader; submitConditions() is the single worker producer (both wait-free triple buffers).
#pragma once
#include <memory>
#include <string>
#include "core/Modulation.h"
#include "core/ParameterRegistry.h"
#include "engine/EngineTypes.h"

namespace cd::engine {

class CircuitEngine {
public:
    CircuitEngine();
    ~CircuitEngine();
    CircuitEngine(const CircuitEngine&) = delete;
    CircuitEngine& operator=(const CircuitEngine&) = delete;

    // Off-thread. Validates the configuration, builds and DC-solves every slot/family/channel
    // path, settles supply and idle thermal equilibrium (Warm start), designs oversamplers and
    // latency for the phase family/zero-latency/pitch-drift selection in `plain`. On failure
    // returns false with a reason and leaves the engine unprepared.
    bool prepare(const EngineConfig& cfg, const core::ParamValues& plain, std::string& err);
    bool isPrepared() const noexcept;
    int latencySamples() const noexcept;

    // Audio thread. frames >= 0 (any count; split internally); 0 frames applies parameter and
    // condition targets only and advances no clock. in/out: `channels` (<= prepared) buffers;
    // in == out aliasing is allowed. sidechain is accepted for API stability: external load
    // detection is owned by cd_control and arrives as mod.externalLoadA.
    void process(const ProcessContext& ctx, const double* const* in, double* const* out, int channels,
                 const double* const* sidechain, int scChannels, int frames,
                 const core::ParamValues& plain, const core::ModulationBlock& mod) noexcept;

    void setBypassed(bool bypassed) noexcept;
    // Warm: prepared idle thermal equilibrium, prepared DC electrical state, zero stochastic
    // history. Cold: ambient temperatures and the ambient-temperature DC state. Both clear the
    // solver/containment fallback (the only re-entry path besides a new prepare()).
    void reset(ResetKind kind) noexcept;
    // True when `plain` requests a phase family, zero-latency or pitch-drift change: staged until
    // the next prepare() (host latency handshake); the active configuration keeps running.
    bool needsReprepare(const core::ParamValues& plain) const noexcept;

    // UI thread (single reader): copies the latest snapshot; false if none published yet.
    bool readTelemetry(TelemetryFrame& out) noexcept;
    // Worker thread (single producer): per-slot condition overlay, installed at the next block.
    bool submitConditions(int slot, const ConditionSet& set) noexcept;

    // Checkpoint: exact continuation of all dynamic state for this prepared configuration.
    std::size_t checkpointSize() const noexcept;      // doubles in Checkpoint::values
    void allocateCheckpoint(Checkpoint& cp) const;    // off-thread: sizes the vectors
    bool getCheckpoint(Checkpoint& cp) const noexcept;  // requires allocated storage
    // Invalid layout, scalar ranges or nested DSP state is rejected without changing live state.
    // Validation and any rollback use prepare-time storage; no allocation occurs here.
    bool setCheckpoint(const Checkpoint& cp) noexcept;

    // Introspection (tests, inspector): targets resolved for the last processed block.
    EffectiveTargets effectiveTargets(int slot) const noexcept;
    int activeOversampling() const noexcept;
    int activeNewtonCap() const noexcept;
    bool qualityTransitionActive() const noexcept;
    bool solverFallbackActive() const noexcept;

    // Test hooks (deterministic fault injection; never used by production adapters).
    void debugForceSolverFailure(bool on) noexcept;
    void debugInjectWetNonFinite(int samples) noexcept;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cd::engine
