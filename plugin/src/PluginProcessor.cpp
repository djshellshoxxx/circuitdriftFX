#include "PluginProcessor.h"

#include "ParameterLayout.h"

namespace cd::plugin {
namespace {
constexpr const char* kStateType = "CircuitDriftState";
}

juce::AudioProcessor::BusesProperties CircuitDriftProcessor::makeBuses() {
    // Main stereo in/out plus an optional (disabled by default) stereo sidechain input.
    return BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
        .withInput("Sidechain", juce::AudioChannelSet::stereo(), false);
}

CircuitDriftProcessor::CircuitDriftProcessor(Edition edition, const juce::String& displayName)
    : juce::AudioProcessor(makeBuses()),
      edition_(edition),
      displayName_(displayName),
      apvts_(*this, nullptr, kStateType, buildParameterLayout(edition)) {}

bool CircuitDriftProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& out = layouts.getMainOutputChannelSet();
    const auto& in = layouts.getMainInputChannelSet();
    const bool monoOrStereo = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    if (!monoOrStereo || in != out) return false;
    const auto& sidechain = layouts.getChannelSet(true, 1);
    return sidechain.isDisabled() || sidechain == juce::AudioChannelSet::mono() ||
           sidechain == juce::AudioChannelSet::stereo();
}

void CircuitDriftProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    sampleRate_ = sampleRate;
    maxBlock_ = maximumExpectedSamplesPerBlock;
    setLatencySamples(0);
}

void CircuitDriftProcessor::releaseResources() {
    sampleRate_ = 0.0;
    maxBlock_ = 0;
}

template <typename Sample>
void CircuitDriftProcessor::passThrough(juce::AudioBuffer<Sample>& buffer) noexcept {
    // Audio is processed in place on the main bus; output channels without a matching input are
    // silenced and the sidechain bus is left untouched. The DSP engine is not connected yet.
    auto main = getBusBuffer(buffer, false, 0);
    const int numIn = getMainBusNumInputChannels();
    for (int ch = numIn; ch < main.getNumChannels(); ++ch) main.clear(ch, 0, main.getNumSamples());
}

void CircuitDriftProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    passThrough(buffer);
}

void CircuitDriftProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    passThrough(buffer);
}

juce::AudioProcessorParameter* CircuitDriftProcessor::getBypassParameter() const {
    return edition_ == Edition::Lite ? apvts_.getParameter(kBypassParamId) : nullptr;
}

juce::AudioProcessorEditor* CircuitDriftProcessor::createEditor() {
    return new juce::GenericAudioProcessorEditor(*this);
}

void CircuitDriftProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (const auto xml = apvts_.copyState().createXml()) copyXmlToBinary(*xml, destData);
}

void CircuitDriftProcessor::setStateInformation(const void* data, int sizeInBytes) {
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml != nullptr && xml->hasTagName(apvts_.state.getType()))
        apvts_.replaceState(juce::ValueTree::fromXml(*xml));
}

}  // namespace cd::plugin
