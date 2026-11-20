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

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>

namespace cd::plugin {

class CircuitDriftProcessor final : public juce::AudioProcessor, private juce::Thread {
public:
    CircuitDriftProcessor(Edition edition, const juce::String& displayName);
    ~CircuitDriftProcessor() override;

    Edition edition() const noexcept { return edition_; }
    juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts_; }
    bool midiOverflowed() const noexcept { return midiOverflowed_.load(std::memory_order_relaxed); }

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
    core::ParamValues parameterSnapshot() const noexcept;
    void run() override;  // bounded pitch work, never on the audio thread
    void stopPitchWorker();
    template <typename Sample>
    void processAudio(juce::AudioBuffer<Sample>& buffer, juce::MidiBuffer& midi, bool bypassed) noexcept;

    Edition edition_;
    juce::String displayName_;
    juce::AudioProcessorValueTreeState apvts_;
    std::array<std::atomic<float>*, core::kParamCount> rawParameters_{};
    std::atomic<float>* rawBypass_ = nullptr;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CircuitDriftProcessor)
};

}  // namespace cd::plugin
