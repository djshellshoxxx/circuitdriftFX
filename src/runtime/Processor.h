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
    bool isPrepared() const noexcept { return prepared_; }
    int latencySamples() const noexcept { return prepared_ ? engine_.latencySamples() : 0; }
    bool needsReprepare(const core::ParamValues& values) const noexcept;

    // Hosts may supply more than maxBlock samples: every sample is processed in
    // bounded chunks. Input/output may alias. Null input channels are silence.
    // MIDI must be sorted, with offsets relative to this call. At most 1024 events
    // are accepted per call; overflow is observable via midiOverflowed() and ends
    // the admitted prefix with all-notes-off so a dropped release cannot latch.
    // Zero frames applies targets without advancing audio/control clocks.
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
};

}  // namespace cd::runtime
