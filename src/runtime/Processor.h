// Framework-independent host integration. Preparation/worker calls are non-audio;
// process() is bounded, noexcept and allocation-free after successful preparation.
// Physical models remain synthetic fixtures (G01 open).
#pragma once
#include <array>
#include <string>
#include <vector>
#include "control/ControlProcessor.h"
#include "engine/CircuitEngine.h"

namespace cd::runtime {

// Normalises host values before any enum/integer conversion. LITE uses the version-1
// mapping from 07-lite-edition.md; full-only controls cannot leak into its sound.
core::ParamValues sanitiseParameters(const core::ParamValues& values) noexcept;
core::ParamValues mapLiteParameters(const core::ParamValues& values) noexcept;

class Processor {
public:
    static constexpr int kMaxBlock = 8192;
    static constexpr std::size_t kMaxMidiEvents = 1024;

    // Reprepare/release/reset must not overlap audio or pitch-worker execution.
    bool prepare(const engine::EngineConfig& config, const core::ParamValues& parameters,
                 bool lite, std::string& error);
    void release() noexcept;
    void reset(engine::ResetKind kind) noexcept;
    // Audio owner only, between process calls. Bounded, allocation-free and
    // safe while the single pitch worker runs; never joins or resets its buffers.
    void resetRealtime(engine::ResetKind kind) noexcept;
    bool isPrepared() const noexcept { return prepared_; }
    int latencySamples() const noexcept { return prepared_ ? engine_.latencySamples() : 0; }
    bool needsReprepare(const core::ParamValues& values) const noexcept;

    // Engine-only continuation bridge for a STOPPED runtime. The caller must
    // quiesce both host audio callbacks and the pitch worker across these calls.
    // Allocation runs off-thread and may throw; capture/restore require that
    // storage to have been preallocated and never allocate on their own.
    // This does NOT capture control/key/tempo/pitch, runtime epoch, host
    // parameters, blueprint, slot seeds or a complete session transaction.
    bool allocateEngineCheckpoint(engine::Checkpoint& cp) const {
        if (!prepared_) return false;
        engine_.allocateCheckpoint(cp);
        return true;
    }
    bool captureEngineCheckpoint(engine::Checkpoint& cp) noexcept {
        return prepared_ && engine_.getCheckpoint(cp);
    }
    bool restoreEngineCheckpoint(const engine::Checkpoint& cp) noexcept {
        return prepared_ && engine_.setCheckpoint(cp);
    }

    // Hosts may supply more than maxBlock samples: every sample is processed in
    // bounded chunks. Input/output may alias. Null input channels are silence.
    // MIDI must be sorted, with offsets relative to this call. At most 1024 events
    // are accepted per call; overflow is observable via midiOverflowed() and ends
    // the admitted prefix with all-notes-off so a dropped release cannot latch.
    // Zero frames applies sample-zero MIDI events and targets without advancing clocks.
    void process(const engine::ProcessContext& context, const control::ControlContext& controls,
                 const double* const* input, double* const* output, int channels,
                 const double* const* sidechain, int sidechainChannels, int frames,
                 const core::ParamValues& parameters, bool bypassed = false) noexcept;

    // One non-audio pitch worker; must be quiescent for prepare/release/reset.
    void runPitchWorker() noexcept { if (prepared_) control_.runPitchWorker(); }
    // Single UI reader. Do not call concurrently with prepare/release.
    bool readTelemetry(engine::TelemetryFrame& out) noexcept { return prepared_ && engine_.readTelemetry(out); }
    // The manifest/adapter supplies a measured budget; zero disables external load.
    void setSidechainBudgetAmps(double value) noexcept;
    bool submitConditions(int slot, const engine::ConditionSet& conditions) noexcept {
        return prepared_ && !lite_ && engine_.submitConditions(slot, conditions);
    }
    // Audio thread diagnostics (adapters publish a separate UI snapshot).
    const control::ControlStatus& controlStatus() const noexcept { return control_.status(); }
    engine::EffectiveTargets effectiveTargets(int slot) const noexcept {
        return prepared_ ? engine_.effectiveTargets(slot) : engine::EffectiveTargets{};
    }
    bool midiOverflowed() const noexcept { return midiOverflowed_; }

private:
    core::ParamValues mapped(const core::ParamValues& values) const noexcept;
    engine::CircuitEngine engine_;
    control::ControlProcessor control_;
    engine::EngineConfig config_{};
    std::vector<double> silence_;
    std::array<control::MidiEvent, kMaxMidiEvents> midi_{};
    bool prepared_ = false, lite_ = false, midiOverflowed_ = false;
    bool activeZeroLatency_ = false, activePitchDrift_ = false;
    bool haveEpoch_ = false;
    uint64_t lastEpoch_ = 0;
};

}  // namespace cd::runtime
