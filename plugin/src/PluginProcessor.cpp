#include "PluginProcessor.h"

#include "ParameterLayout.h"
#include "EditorState.h"
#include "ui/PluginEditor.h"
#include "ui/ProcessorEditorBridge.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace cd::plugin {
namespace {
constexpr const char* kStateType = "CircuitDriftState";
constexpr int kMaxStateBytes = 4 * 1024 * 1024;
constexpr const char* kAdapterStateVersion = "1";
constexpr const char* kLiteMappingVersion = "1";
const char* editionName(Edition edition) noexcept { return edition == Edition::Lite ? "lite" : "full"; }
std::int64_t advanceSamples(std::int64_t sample, int frames) noexcept {
    return sample > std::numeric_limits<std::int64_t>::max() - frames
        ? std::numeric_limits<std::int64_t>::max() : sample + frames;
}
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
      juce::Thread("CircuitDrift pitch"),
      edition_(edition),
      displayName_(displayName),
      apvts_(*this, nullptr, kStateType, buildParameterLayout(edition)) {
    static_assert(std::atomic<float>::is_always_lock_free, "Audio parameters must be lock-free");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "Audio commands must be lock-free");
    for (std::size_t i = 0; i < core::kParamCount; ++i)
        if (isExposed(core::kParams[i], edition_)) rawParameters_[i] = apvts_.getRawParameterValue(core::kParams[i].id);
    if (edition_ == Edition::Lite) rawBypass_ = apvts_.getRawParameterValue(kBypassParamId);
}

CircuitDriftProcessor::~CircuitDriftProcessor() { releaseResources(); }

core::ParamValues CircuitDriftProcessor::parameterSnapshot() const noexcept {
    auto values = core::defaultValues();
    for (std::size_t i = 0; i < values.size(); ++i)
        if (rawParameters_[i]) values[i] = rawParameters_[i]->load(std::memory_order_relaxed);
    return values;
}

void CircuitDriftProcessor::run() {
    while (!threadShouldExit()) {
        runtime_.runPitchWorker();
        wait(10);
    }
}

const CircuitDriftProcessor::EditorSnapshot* CircuitDriftProcessor::latestEditorSnapshot() noexcept {
    if (!editorReadsEnabled_.load(std::memory_order_acquire)) return nullptr;
    editorSnapshots_.acquire();
    const auto& snapshot = editorSnapshots_.front();
    if (snapshot.generation != editorGeneration_.load(std::memory_order_acquire) ||
        !editorReadsEnabled_.load(std::memory_order_acquire)) return nullptr;
    return &snapshot;
}

bool CircuitDriftProcessor::readEditorSnapshot(EditorSnapshot& out) noexcept {
    if (const auto* snapshot = latestEditorSnapshot()) { out = *snapshot; return true; }
    return false;
}

bool CircuitDriftProcessor::readEditorTelemetry(engine::TelemetryFrame& out) noexcept {
    if (const auto* snapshot = latestEditorSnapshot()) { out = snapshot->telemetry; return true; }
    return false;
}

void CircuitDriftProcessor::publishEditorSnapshot(double bpm) noexcept {
    auto& snapshot = editorSnapshots_.back();
    if (!runtime_.readTelemetry(snapshot.telemetry)) return;
    snapshot.generation = editorGeneration_.load(std::memory_order_relaxed);
    snapshot.controls = runtime_.controlStatus();
    for (int slot = 0; slot < engine::kSlots; ++slot) snapshot.targets[slot] = runtime_.effectiveTargets(slot);
    snapshot.bpm = bpm;
    editorSnapshots_.publish();
}

bool CircuitDriftProcessor::requestEngineReset(bool cold) noexcept {
    const auto generation = editorGeneration_.load(std::memory_order_acquire);
    if (!editorReadsEnabled_.load(std::memory_order_acquire)) return false;
    // A generation stamp prevents a request racing release/reprepare from
    // resetting a different prepared instance. Repeated clicks coalesce.
    pendingReset_.store((generation << 2) | (cold ? 2u : 1u), std::memory_order_release);
    return true;
}

UiSettings CircuitDriftProcessor::editorSettings() const {
    const std::lock_guard lock(editorSettingsMutex_);
    return editorSettings_;
}

void CircuitDriftProcessor::setEditorSettings(const UiSettings& settings) {
    const auto next = boundedEditorSettings(settings);
    {
        const std::lock_guard lock(editorSettingsMutex_);
        if (sameEditorSettings(editorSettings_, next)) return;
        editorSettings_ = next;
    }
    // Hosts need an explicit dirty-state notification for persisted preferences
    // that are deliberately not automatable sound parameters. Notify unlocked.
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}

void CircuitDriftProcessor::stopPitchWorker() {
    signalThreadShouldExit();
    notify();
    stopThread(-1);  // prepare/release/destruction are non-audio and wait for bounded worker work
}

bool CircuitDriftProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    if (layouts.inputBuses.size() != 2 || layouts.outputBuses.size() != 1) return false;
    const auto& out = layouts.getMainOutputChannelSet();
    const auto& in = layouts.getMainInputChannelSet();
    const bool monoOrStereo = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    if (!monoOrStereo || in != out) return false;
    const auto& sidechain = layouts.getChannelSet(true, 1);
    return sidechain.isDisabled() || sidechain == juce::AudioChannelSet::mono() ||
           sidechain == juce::AudioChannelSet::stereo();
}

void CircuitDriftProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    releaseResources();
    prepareError_.clear();
    if (!std::isfinite(sampleRate) || sampleRate <= 0) return;
    engine::EngineConfig config;
    config.sampleRate = sampleRate;
    config.maxBlock = std::clamp(maximumExpectedSamplesPerBlock, 1, runtime::Processor::kMaxBlock);
    config.channels = getMainBusNumOutputChannels();
    if (config.channels < 1 || config.channels > engine::kMaxChannels) return;
    for (auto& channel : audioScratch_) channel.assign(static_cast<std::size_t>(config.maxBlock), 0.0);
    for (auto& channel : sidechainScratch_) channel.assign(static_cast<std::size_t>(config.maxBlock), 0.0);
    if (!runtime_.prepare(config, parameterSnapshot(), edition_ == Edition::Lite, prepareError_)) return;
    sampleRate_ = sampleRate;
    maxBlock_ = config.maxBlock;
    sampleCursor_ = expectedHostSample_ = 0;
    epoch_ = 0;
    haveHostPosition_ = false;
    midiOverflowed_.store(false, std::memory_order_relaxed);
    setLatencySamples(runtime_.latencySamples());
    if (!startThread()) {
        prepareError_ = "Could not start pitch-analysis worker";
        runtime_.release();
        sampleRate_ = 0;
        maxBlock_ = 0;
        setLatencySamples(0);
    } else {
        editorSampleRate_.store(sampleRate, std::memory_order_release);
        editorChannels_.store(config.channels, std::memory_order_release);
        editorLatency_.store(runtime_.latencySamples(), std::memory_order_release);
        editorReadsEnabled_.store(true, std::memory_order_release);
    }
}

void CircuitDriftProcessor::releaseResources() {
    editorReadsEnabled_.store(false, std::memory_order_release);
    editorGeneration_.fetch_add(1, std::memory_order_acq_rel);
    pendingReset_.store(0, std::memory_order_release);
    stopPitchWorker();
    runtime_.release();
    sampleRate_ = 0.0;
    maxBlock_ = 0;
    editorSampleRate_.store(0.0, std::memory_order_release);
    editorLatency_.store(0, std::memory_order_release);
    setLatencySamples(0);
}

template <typename Sample>
void CircuitDriftProcessor::processAudio(juce::AudioBuffer<Sample>& buffer, juce::MidiBuffer& midi, bool bypassed) noexcept {
    const int frames = buffer.getNumSamples();
    if (frames < 0) { midi.clear(); return; }
    const int channels = std::min(getMainBusNumOutputChannels(), engine::kMaxChannels);
    if (!runtime_.isPrepared() || maxBlock_ <= 0 ||
        (frames > 0 && buffer.getNumChannels() < std::max(getTotalNumInputChannels(), getTotalNumOutputChannels()))) {
        for (int ch = 0; ch < std::min(channels, buffer.getNumChannels()); ++ch) buffer.clear(ch, 0, frames);
        midi.clear();
        return;
    }
    const auto reset = pendingReset_.exchange(0, std::memory_order_acq_rel);
    const auto generation = editorGeneration_.load(std::memory_order_relaxed);
    if (reset != 0 && (reset & ~std::uint64_t{3}) == (generation << 2))
        runtime_.resetRealtime((reset & 3u) == 2u ? engine::ResetKind::Cold : engine::ResetKind::Warm);
    // A parameter-only callback may have no audio channel pointers. Do not form
    // JUCE bus views until positive frame count and the complete bus layout exist.
    auto output = frames > 0 ? getBusBuffer(buffer, false, 0) : juce::AudioBuffer<Sample>{};
    auto input = frames > 0 ? getBusBuffer(buffer, true, 0) : juce::AudioBuffer<Sample>{};
    auto sidechain = frames > 0 ? getBusBuffer(buffer, true, 1) : juce::AudioBuffer<Sample>{};
    const int sidechainChannels = std::min(sidechain.getNumChannels(), engine::kMaxChannels);
    const auto parameters = parameterSnapshot();
    bypassed = bypassed || (rawBypass_ && rawBypass_->load(std::memory_order_relaxed) >= 0.5f);

    control::ControlContext controls;
    std::int64_t blockStart = sampleCursor_;
    bool hostPosition = false;
    if (auto* playHead = getPlayHead()) {
        if (const auto position = playHead->getPosition()) {
            controls.playing = position->getIsPlaying();
            if (const auto bpm = position->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                controls.bpm = std::clamp(*bpm, 20.0, 300.0);
            if (const auto ppq = position->getPpqPosition(); ppq && std::isfinite(*ppq)) controls.ppq = *ppq;
            if (const auto signature = position->getTimeSignature()) {
                controls.timeSigNum = std::clamp(signature->numerator, 1, 64);
                controls.timeSigDen = std::clamp(signature->denominator, 1, 64);
            }
            if (const auto hostSample = position->getTimeInSamples()) {
                blockStart = *hostSample;
                hostPosition = true;
                if (haveHostPosition_ && blockStart != expectedHostSample_) ++epoch_;
                expectedHostSample_ = controls.playing ? advanceSamples(blockStart, frames) : blockStart;
            }
        }
    }
    haveHostPosition_ = hostPosition;
    midiOverflowed_.store(false, std::memory_order_relaxed);
    std::size_t midiCount = 0, inspected = 0;
    for (const auto metadata : midi) {
        if (++inspected > midiEvents_.size()) {
            midiOverflowed_.store(true, std::memory_order_relaxed);
            // A dropped note-off must not leave key tracking latched indefinitely. End the
            // admitted event prefix with one all-notes-off (the control tracker clears all channels).
            const auto frame = midiCount == 0 ? 0u : midiEvents_[midiCount - 1].frame;
            if (midiCount == midiEvents_.size()) --midiCount;
            midiEvents_[midiCount++] = {frame, 0xb0, 123, 0};
            break;  // bound host-provided MIDI work, including SysEx
        }
        if (metadata.numBytes < 3 || metadata.samplePosition < 0 || metadata.samplePosition >= std::max(frames, 1)) continue;
        const auto kind = metadata.data[0] & 0xf0;
        if (kind != 0x80 && kind != 0x90 && kind != 0xb0) continue;
        midiEvents_[midiCount++] = {static_cast<std::uint32_t>(metadata.samplePosition), metadata.data[0],
                                  static_cast<std::uint8_t>(metadata.data[1] & 0x7f), static_cast<std::uint8_t>(metadata.data[2] & 0x7f)};
    }
    midi.clear();  // this effect consumes MIDI and never produces it

    std::size_t midiCursor = 0;
    int offset = 0;
    do {
        const int count = std::min(maxBlock_, frames - offset);
        const double* inputs[engine::kMaxChannels]{};
        double* outputs[engine::kMaxChannels]{};
        const double* sidechains[engine::kMaxChannels]{};
        for (int ch = 0; ch < channels; ++ch) {
            auto* samples = audioScratch_[static_cast<std::size_t>(ch)].data();
            for (int i = 0; i < count; ++i)
                samples[i] = ch < input.getNumChannels() ? static_cast<double>(input.getSample(ch, offset + i)) : 0.0;
            inputs[ch] = samples;
            outputs[ch] = samples;
        }
        for (int ch = 0; ch < sidechainChannels; ++ch) {
            auto* samples = sidechainScratch_[static_cast<std::size_t>(ch)].data();
            for (int i = 0; i < count; ++i) samples[i] = static_cast<double>(sidechain.getSample(ch, offset + i));
            sidechains[ch] = samples;
        }
        auto chunkControls = controls;
        const auto chunkStart = advanceSamples(blockStart, offset);
        chunkControls.sampleStart = static_cast<std::uint64_t>(std::max<std::int64_t>(0, chunkStart));
        chunkControls.frames = count;
        if (chunkControls.ppq && chunkControls.playing)
            *chunkControls.ppq += static_cast<double>(offset) / sampleRate_ * chunkControls.bpm.value_or(120.0) / 60.0;
        std::size_t chunkEvents = 0;
        while (midiCursor < midiCount && midiEvents_[midiCursor].frame < static_cast<std::uint32_t>(offset + std::max(count, 1))) {
            auto event = midiEvents_[midiCursor++];
            if (event.frame < static_cast<std::uint32_t>(offset)) continue;
            event.frame -= static_cast<std::uint32_t>(offset);
            chunkMidi_[chunkEvents++] = event;
        }
        chunkControls.midi = {chunkMidi_.data(), chunkEvents};
        engine::ProcessContext context;
        context.sampleRate = sampleRate_;
        context.sampleStart = chunkStart;
        context.epoch = epoch_;
        context.frames = static_cast<std::uint32_t>(count);
        context.offline = isNonRealtime();
        context.transport = {controls.playing, controls.bpm.value_or(0), chunkControls.ppq.value_or(0), chunkControls.ppq.has_value()};
        runtime_.process(context, chunkControls, inputs, outputs, channels, sidechains, sidechainChannels, count, parameters, bypassed);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < count; ++i)
                output.setSample(ch, offset + i, static_cast<Sample>(audioScratch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)]));
        offset += count;
    } while (offset < frames);
    for (int ch = channels; ch < output.getNumChannels(); ++ch) output.clear(ch, 0, frames);
    sampleCursor_ = advanceSamples(blockStart, frames);
    publishEditorSnapshot(controls.bpm.value_or(120.0));
}

void CircuitDriftProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    processAudio(buffer, midi, false);
}

void CircuitDriftProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    processAudio(buffer, midi, false);
}

void CircuitDriftProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    processAudio(buffer, midi, true);
}

void CircuitDriftProcessor::processBlockBypassed(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    processAudio(buffer, midi, true);
}

juce::AudioProcessorParameter* CircuitDriftProcessor::getBypassParameter() const {
    return edition_ == Edition::Lite ? apvts_.getParameter(kBypassParamId) : nullptr;
}

juce::AudioProcessorEditor* CircuitDriftProcessor::createEditor() {
    if (!editorBridge_) editorBridge_ = std::make_unique<ProcessorEditorBridge>(*this);
    return new ui::CircuitDriftEditor(*this, *editorBridge_);
}

void CircuitDriftProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts_.copyState();
    state.setProperty("schemaVersion", kAdapterStateVersion, nullptr);
    state.setProperty("edition", editionName(edition_), nullptr);
    if (edition_ == Edition::Lite) state.setProperty("liteMappingVersion", kLiteMappingVersion, nullptr);
    if (const auto xml = state.createXml()) {
        writeEditorSettings(*xml, editorSettings());
        copyXmlToBinary(*xml, destData);
    }
}

void CircuitDriftProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes <= 0 || sizeInBytes > kMaxStateBytes) return;
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(apvts_.state.getType())) return;
    if (xml->hasAttribute("schemaVersion") && xml->getStringAttribute("schemaVersion") != kAdapterStateVersion) return;
    if (xml->hasAttribute("edition") && xml->getStringAttribute("edition") != editionName(edition_)) return;
    if (edition_ == Edition::Lite && xml->hasAttribute("liteMappingVersion") &&
        xml->getStringAttribute("liteMappingVersion") != kLiteMappingVersion) return;
    UiSettings settings;
    if (!readEditorSettings(*xml, settings)) return;

    // Legacy APVTS states have no edition marker. Accept only an unambiguous
    // subset of this edition's known parameters; reject mixed/unknown payloads.
    // Validate the entire candidate before JUCE can publish any parameter value.
    std::array<juce::RangedAudioParameter*, core::kParamCount + 1> seen{};
    std::size_t count = 0;
    for (const auto* child : xml->getChildIterator()) {
        if (!child->hasTagName("PARAM") || count == seen.size()) return;
        auto* parameter = apvts_.getParameter(child->getStringAttribute("id"));
        if (!parameter || std::find(seen.begin(), seen.begin() + count, parameter) != seen.begin() + count) return;
        const auto encoded = child->getStringAttribute("value").toStdString();
        // APVTS stores float values. Parse directly to that representation so a
        // decimal serialization of a float endpoint round-trips exactly.
        float value = 0;
        const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), value);
        const auto& range = parameter->getNormalisableRange();
        if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + encoded.size() || !std::isfinite(value) ||
            value < range.start || value > range.end || (parameter->isDiscrete() && value != std::round(value))) return;
        seen[count++] = parameter;
    }
    if (count == 0) return;
    apvts_.replaceState(juce::ValueTree::fromXml(*xml));
    {
        const std::lock_guard lock(editorSettingsMutex_);
        editorSettings_ = settings;  // Loading host state is not a new user edit.
    }
    editorStateEpoch_.fetch_add(1, std::memory_order_release);
}

}  // namespace cd::plugin
