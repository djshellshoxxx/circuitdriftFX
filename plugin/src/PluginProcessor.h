// CircuitDrift plugin adapter: JUCE AudioProcessor shell shared by the full and LITE products.
//
// Adapts host parameters, audio, MIDI and transport to the framework-independent runtime.
// Circuit models remain synthetic fixtures (G01 open); custom editor/services are separate work.
//
// Ownership: owns the parameter tree (APVTS). Threading: constructor, prepareToPlay, releaseResources,
// get/setStateInformation run on the host's non-audio threads; processBlock runs on the audio
// thread and performs no allocation, locking, I/O or logging.
#pragma once

#include "Edition.h"
#include "runtime/Processor.h"
#include "engine/Realtime.h"
#include "ui/EditorBridge.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace cd::plugin {

class EditorBridge;
class CircuitDriftProcessor final : public juce::AudioProcessor, private juce::Thread {
public:
    CircuitDriftProcessor(Edition edition, const juce::String& displayName);
    ~CircuitDriftProcessor() override;

    Edition edition() const noexcept { return edition_; }
    juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts_; }
    bool midiOverflowed() const noexcept { return midiOverflowed_.load(std::memory_order_relaxed); }
    struct EditorSnapshot {
        std::uint64_t generation = 0;
        engine::TelemetryFrame telemetry{};
        control::ControlStatus controls{};
        engine::EffectiveTargets targets[engine::kSlots]{};
        double bpm = 120.0;
    };
    // Single message-thread reader. Snapshot storage outlives runtime preparation,
    // so neither the UI nor host lifecycle waits for the other to release a read.
    bool readEditorSnapshot(EditorSnapshot& out) noexcept;
    bool readEditorTelemetry(engine::TelemetryFrame& out) noexcept;
    // Message thread: the latest request is applied at the next audio boundary.
    // Returns false while unprepared; no audio or pitch-worker state is touched here.
    bool requestEngineReset(bool cold) noexcept;
    // Short copies under a non-audio lock; host saves can overlap message-thread edits.
    UiSettings editorSettings() const;
    void setEditorSettings(const UiSettings& settings);
    int editorLatency() const noexcept { return editorLatency_.load(std::memory_order_acquire); }
    int editorChannels() const noexcept { return editorChannels_.load(std::memory_order_acquire); }
    double editorSampleRate() const noexcept { return editorSampleRate_.load(std::memory_order_acquire); }
    std::uint64_t editorStateEpoch() const noexcept { return editorStateEpoch_.load(std::memory_order_acquire); }

    // --- juce::AudioProcessor ---
    const juce::String getName() const override { return displayName_; }
    bool acceptsMidi() const override { return true; }  // MIDI note input feeds key tracking
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool supportsDoublePrecisionProcessing() const override { return true; }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    static BusesProperties makeBuses();
    struct ParameterSnapshot {
        core::ParamValues values = core::defaultValues();
        bool bypassed = false;  // LITE wrapper parameter, outside the core registry
    };
    ParameterSnapshot parameterSnapshot() const noexcept;
    const ParameterSnapshot& audioParameterSnapshot() noexcept;
    void run() override;  // bounded pitch work, never on the audio thread
    void stopPitchWorker();
    const EditorSnapshot* latestEditorSnapshot() noexcept;
    void publishEditorSnapshot(double bpm) noexcept;
    template <typename Sample>
    void processAudio(juce::AudioBuffer<Sample>& buffer, juce::MidiBuffer& midi, bool bypassed) noexcept;

    Edition edition_;
    juce::String displayName_;
    juce::AudioProcessorValueTreeState apvts_;
    std::array<std::atomic<float>*, core::kParamCount> rawParameters_{};
    std::atomic<float>* rawBypass_ = nullptr;
    // Non-audio host publishers/lifecycle only. Recursive access permits an APVTS
    // notification to save state; a nested restore is rejected below.
    mutable std::recursive_mutex hostStateMutex_;
    bool replacingHostState_ = false;
    // The previous complete host state remains available to reentrant saves and
    // after an exceptional JUCE notification. Never read these fields on audio.
    juce::ValueTree hostStateFallback_;
    UiSettings hostStateFallbackSettings_{};
    ParameterSnapshot hostStateFallbackParameters_{};
    // Odd while replacement is incomplete, even after successful publication.
    // Only the non-audio publisher writes this sequence; audio makes one attempt.
    std::atomic<std::uint64_t> hostStateSequence_{0};
    // Serialized host publisher -> one audio consumer. Publish the complete
    // fallback before the odd sequence, including a successful stopped-audio
    // restore that has not reached a callback yet. No lifecycle reset is needed.
    engine::TripleBuffer<ParameterSnapshot> hostFallbackSnapshots_;
    // Audio-owned while prepared; preparation initializes it with audio quiescent.
    ParameterSnapshot audioParameters_{};
    runtime::Processor runtime_;
    std::array<std::vector<double>, engine::kMaxChannels> audioScratch_, sidechainScratch_;
    std::array<control::MidiEvent, runtime::Processor::kMaxMidiEvents> midiEvents_{}, chunkMidi_{};
    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    std::int64_t sampleCursor_ = 0, expectedHostSample_ = 0;
    std::uint64_t epoch_ = 0;
    bool haveHostPosition_ = false;
    std::atomic<bool> midiOverflowed_{false};
    std::string prepareError_;
    std::unique_ptr<EditorBridge> editorBridge_;
    mutable std::mutex editorSettingsMutex_;
    UiSettings editorSettings_{};
    engine::TripleBuffer<EditorSnapshot> editorSnapshots_;
    std::atomic<bool> editorReadsEnabled_{false};
    std::atomic<std::uint64_t> editorGeneration_{0}, pendingReset_{0};
    std::atomic<double> editorSampleRate_{0.0};
    std::atomic<int> editorChannels_{2}, editorLatency_{0};
    std::atomic<std::uint64_t> editorStateEpoch_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CircuitDriftProcessor)
};

}  // namespace cd::plugin
