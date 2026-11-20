#include "runtime/Processor.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace cd::runtime {
namespace {
void set(core::ParamValues& values, std::string_view id, double value) noexcept {
    const auto index = core::indexOf(id);
    if (index < values.size()) values[index] = value;
}
void silence(double* const* output, int channels, int frames) noexcept {
    if (!output || frames <= 0) return;
    for (int ch = 0; ch < std::clamp(channels, 0, engine::kMaxChannels); ++ch)
        if (output[ch]) std::fill_n(output[ch], frames, 0.0);
}
int64_t offsetSample(int64_t sample, int offset) noexcept {
    const auto max = std::numeric_limits<int64_t>::max();
    return sample > max - offset ? max : sample + offset;
}
}

core::ParamValues sanitiseParameters(const core::ParamValues& values) noexcept {
    core::ParamValues result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& spec = core::kParams[i];
        const double value = std::isfinite(values[i]) ? values[i] : spec.def;
        result[i] = std::clamp(value, spec.min, spec.max);
        if (spec.kind != core::ParamKind::Float) result[i] = std::round(result[i]);
    }
    return result;
}

core::ParamValues mapLiteParameters(const core::ParamValues& values) noexcept {
    const auto clean = sanitiseParameters(values);
    auto result = core::defaultValues();
    const double sag = clean[core::indexOf("lite.sag")];
    const double age = clean[core::indexOf("lite.thermalAge")];
    set(result, "lite.sag", sag);
    set(result, "lite.thermalAge", age);
    set(result, "macro.sag", sag);
    set(result, "macro.age", age);
    set(result, "macro.drift", 0.25 * age);
    set(result, "macro.warmth", 0.2);
    set(result, "gainMatch.enabled", 1.0);
    set(result, "quality.mode", 0.0);  // Eco, auto 4x FIR
    // Remaining defaults already implement fixed wet mix, 0 dB trims, 15 Hz HPF,
    // soft clip, noise off, slot A only, and creative pitch delay disabled.
    return result;
}

core::ParamValues Processor::mapped(const core::ParamValues& values) const noexcept {
    return lite_ ? mapLiteParameters(values) : sanitiseParameters(values);
}

bool Processor::prepare(const engine::EngineConfig& config, const core::ParamValues& parameters,
                        bool lite, std::string& error) {
    prepared_ = false;
    if (!std::isfinite(config.sampleRate) || config.sampleRate < 8000 || config.sampleRate > 768000 ||
        config.maxBlock < 1 || config.maxBlock > kMaxBlock || config.channels < 1 || config.channels > 2) {
        error = "runtime requires 8..768 kHz, 1..8192 frames and mono/stereo channels";
        return false;
    }
    lite_ = lite;
    const auto values = mapped(parameters);
    try {
        if (!engine_.prepare(config, values, error)) return false;
        control_.prepare(config.sampleRate, config.maxBlock, config.seed);
        silence_.assign(static_cast<std::size_t>(config.maxBlock), 0.0);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    config_ = config;
    activeZeroLatency_ = values[core::indexOf("quality.zeroLatency")] != 0;
    activePitchDrift_ = values[core::indexOf("pitchDrift.enabled")] != 0;
    midiOverflowed_ = false;
    haveEpoch_ = false;
    lastEpoch_ = 0;
    prepared_ = true;
    error.clear();
    return true;
}

void Processor::release() noexcept { prepared_ = false; haveEpoch_ = false; }
void Processor::reset(engine::ResetKind kind) noexcept {
    if (!prepared_) return;
    control_.reset();
    engine_.reset(kind);
    haveEpoch_ = false;
}
bool Processor::needsReprepare(const core::ParamValues& values) const noexcept {
    return !prepared_ || engine_.needsReprepare(mapped(values));
}
void Processor::setSidechainBudgetAmps(double value) noexcept {
    control_.setSidechainBudgetA(std::isfinite(value) && value > 0 ? value : 0.0);
}

void Processor::process(const engine::ProcessContext& context, const control::ControlContext& controls,
                        const double* const* input, double* const* output, int channels,
                        const double* const* sidechain, int sidechainChannels, int frames,
                        const core::ParamValues& parameters, bool bypassed) noexcept {
    if (frames < 0) return;
    if (!prepared_ || channels < 1 || channels > config_.channels ||
        (frames > 0 && (!output || !output[0] || (channels > 1 && !output[1])))) {
        silence(output, channels, frames);
        return;
    }
    // Host seek/loop discontinuities are reported by the adapter as epoch
    // changes. Only an atomic request reaches the pitch worker here: never
    // reset or re-prepare its analysis state on the audio thread.
    if (haveEpoch_ && context.epoch != lastEpoch_) control_.markPitchDiscontinuity();
    lastEpoch_ = context.epoch;
    haveEpoch_ = true;
    const auto values = mapped(parameters);
    // The engine stages these modes until the host can accept the new latency.
    // Its modulation producer must keep using that same active configuration.
    auto controlValues = values;
    set(controlValues, "quality.zeroLatency", activeZeroLatency_ ? 1.0 : 0.0);
    set(controlValues, "pitchDrift.enabled", activePitchDrift_ ? 1.0 : 0.0);
    engine_.setBypassed(bypassed);
    midiOverflowed_ = controls.midi.size() > kMaxMidiEvents;
    // LITE ignores MIDI entirely, including its effect on processing boundaries.
    std::size_t sourceCount = lite_ ? 0 : std::min(controls.midi.size(), kMaxMidiEvents);
    const bool releaseOverflow = midiOverflowed_ && !lite_;
    control::MidiEvent releaseEvent{};
    if (releaseOverflow) {
        sourceCount = kMaxMidiEvents - 1;
        const auto frame = std::min(controls.midi[sourceCount].frame, static_cast<uint32_t>(std::max(0, frames - 1)));
        // Out-of-block events cannot put the terminal release beyond this call.
        while (sourceCount > 0 && controls.midi[sourceCount - 1].frame > frame) --sourceCount;
        releaseEvent = {frame, 0xb0, 123, 0};
    }
    const std::size_t eventLimit = sourceCount + (releaseOverflow ? 1 : 0);
    const auto eventAt = [&](std::size_t index) -> const control::MidiEvent& {
        return index < sourceCount ? controls.midi[index] : releaseEvent;
    };
    std::size_t eventIndex = 0;
    int offset = 0;
    // A zero-frame call applies sample-zero events and targets without advancing time.
    do {
        int count = std::min(frames - offset, config_.maxBlock);
        // Apply a note at its sample boundary, never at the start of the earlier
        // control chunk. Equal-offset events remain ordered and are applied together.
        while (eventIndex < eventLimit && eventAt(eventIndex).frame < uint32_t(offset)) ++eventIndex;
        auto nextEvent = eventIndex;
        while (nextEvent < eventLimit && eventAt(nextEvent).frame == uint32_t(offset)) ++nextEvent;
        if (count > 0 && nextEvent < eventLimit &&
            eventAt(nextEvent).frame > uint32_t(offset) &&
            eventAt(nextEvent).frame < uint32_t(offset + count))
            count = int(eventAt(nextEvent).frame - uint32_t(offset));
        const double* in[2] = {silence_.data(), silence_.data()};
        double* out[2] = {nullptr, nullptr};
        const double* sc[2] = {nullptr, nullptr};
        for (int ch = 0; ch < channels; ++ch) {
            if (input && input[ch]) in[ch] = input[ch] + offset;
            if (output && output[ch]) out[ch] = output[ch] + offset;
        }
        const int scCount = lite_ ? 0 : std::clamp(sidechainChannels, 0, 2);
        for (int ch = 0; ch < scCount; ++ch)
            if (sidechain && sidechain[ch]) sc[ch] = sidechain[ch] + offset;

        auto cc = controls;
        cc.frames = count;
        const auto maxSample = std::numeric_limits<uint64_t>::max();
        cc.sampleStart = controls.sampleStart > maxSample - uint64_t(offset)
            ? maxSample : controls.sampleStart + uint64_t(offset);
        if (cc.bpm && (!std::isfinite(*cc.bpm) || *cc.bpm <= 0)) cc.bpm.reset();
        if (cc.bpm) *cc.bpm = control::TempoBreathing::clampBpm(*cc.bpm);
        if (cc.ppq && !std::isfinite(*cc.ppq)) cc.ppq.reset();
        if (cc.ppq && offset > 0 && cc.playing)
            *cc.ppq += double(offset) * cc.bpm.value_or(120.0) / (60.0 * config_.sampleRate);
        std::size_t eventCount = 0;
        while (eventIndex < eventLimit) {
            const auto& event = eventAt(eventIndex);
            if (event.frame >= uint32_t(offset + std::max(count, 1))) break;
            ++eventIndex;
            if (event.frame < uint32_t(offset) || lite_) continue;
            midi_[eventCount] = event;
            midi_[eventCount++].frame -= uint32_t(offset);
        }
        cc.midi = std::span<const control::MidiEvent>(midi_.data(), eventCount);
        const control::AudioView audio{in, channels};
        const control::AudioView external{sc, scCount};
        core::ModulationBlock modulation;
        control_.processBlock(controlValues, cc, audio, scCount > 0 ? &external : nullptr, modulation);
        auto ec = context;
        ec.sampleRate = config_.sampleRate;
        ec.sampleStart = offsetSample(context.sampleStart, offset);
        ec.frames = static_cast<uint32_t>(count);
        ec.transport.playing = cc.playing;
        ec.transport.bpm = cc.bpm.value_or(0.0);
        ec.transport.ppqValid = cc.ppq.has_value();
        ec.transport.ppq = cc.ppq.value_or(0.0);
        engine_.process(ec, in, out, channels, sc, scCount, count, values, modulation);
        offset += count;
    } while (offset < frames);
}

}  // namespace cd::runtime
