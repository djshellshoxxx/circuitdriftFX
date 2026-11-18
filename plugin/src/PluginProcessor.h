// CircuitDrift plugin adapter: JUCE AudioProcessor shell shared by the full and LITE products.
//
// Status: the DSP engine (cd_dsp) is not wired in yet. processBlock is an explicit pass-through
// so the plugin can be loaded, automated and state-saved by hosts while the engine matures.
//
// Ownership: owns the parameter tree (APVTS). Threading: constructor, prepareToPlay, releaseResources,
// get/setStateInformation run on the host's non-audio threads; processBlock runs on the audio
// thread and performs no allocation, locking, I/O or logging.
#pragma once

#include "Edition.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace cd::plugin {

class CircuitDriftProcessor final : public juce::AudioProcessor {
public:
    CircuitDriftProcessor(Edition edition, const juce::String& displayName);
    ~CircuitDriftProcessor() override = default;

    Edition edition() const noexcept { return edition_; }
    juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts_; }

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
    template <typename Sample>
    void passThrough(juce::AudioBuffer<Sample>& buffer) noexcept;

    Edition edition_;
    juce::String displayName_;
    juce::AudioProcessorValueTreeState apvts_;
    double sampleRate_ = 0.0;
    int maxBlock_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CircuitDriftProcessor)
};

}  // namespace cd::plugin
