// Host integration regressions, synthetic fixtures only. No hardware/host claim.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>
#include "runtime/Processor.h"

namespace {
thread_local bool countAllocations = false;
thread_local unsigned allocations = 0;
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); } } while (false)
using namespace cd;
void set(core::ParamValues& values, const char* id, double value) { values[core::indexOf(id)] = value; }

struct Rig {
    runtime::Processor processor;
    engine::EngineConfig config;
    engine::ProcessContext ec;
    control::ControlContext cc;
    core::ParamValues parameters = core::defaultValues();
    Rig() { config.channels = 1; config.maxBlock = 64; config.sampleRate = 48000; ec.sampleRate = 48000; }
    bool prepare(bool lite = false) {
        std::string error;
        const bool ok = processor.prepare(config, parameters, lite, error);
        if (!ok) std::fprintf(stderr, "prepare: %s\n", error.c_str());
        return ok;
    }
    std::vector<double> render(const std::vector<double>& input, bool bypass = false) {
        std::vector<double> output(input.size(), -100.0);
        const double* in[] = {input.data()};
        double* out[] = {output.data()};
        processor.process(ec, cc, in, out, 1, nullptr, 0, int(input.size()), parameters, bypass);
        ec.sampleStart += int64_t(input.size());
        cc.sampleStart += uint64_t(input.size());
        return output;
    }
};

void parameterBoundaries() {
    auto p = core::defaultValues();
    set(p, "quality.mode", 1e300);
    set(p, "key.mask", -1e300);
    set(p, "mix", std::numeric_limits<double>::quiet_NaN());
    set(p, "input.gain", std::numeric_limits<double>::infinity());
    auto clean = runtime::sanitiseParameters(p);
    CHECK(clean[core::indexOf("quality.mode")] == 2);
    CHECK(clean[core::indexOf("key.mask")] == 1);  // registry requires at least one scale note
    CHECK(clean[core::indexOf("mix")] == 1);
    CHECK(clean[core::indexOf("input.gain")] == 0);
    set(p, "lite.sag", 0.7);
    set(p, "lite.thermalAge", 0.8);
    set(p, "macro.warmth", 1);
    set(p, "pitchDrift.enabled", 1);
    auto lite = runtime::mapLiteParameters(p);
    CHECK(lite[core::indexOf("macro.sag")] == 0.7);
    CHECK(lite[core::indexOf("macro.age")] == 0.8);
    CHECK(lite[core::indexOf("macro.drift")] == 0.2);
    CHECK(lite[core::indexOf("macro.warmth")] == 0.2);
    CHECK(lite[core::indexOf("pitchDrift.enabled")] == 0);
    CHECK(lite[core::indexOf("gainMatch.enabled")] == 1);
}

void dryLatencyAndLargeBlocks() {
    Rig rig;
    set(rig.parameters, "mix", 0);
    set(rig.parameters, "hpf.enabled", 0);
    set(rig.parameters, "output.softClip", 0);
    set(rig.parameters, "quality.mode", 0);
    CHECK(rig.prepare());
    const int latency = rig.processor.latencySamples();
    CHECK(latency > 0);
    std::vector<double> input(2048);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = 0.1 * std::sin(double(i) * 0.07);
    auto output = rig.render(input);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double expected = int(i) < latency ? 0.0 : input[i - std::size_t(latency)];
        CHECK(std::fabs(output[i] - expected) < 1e-10);
    }
    // No callback allocation, including the oversized-block and null-input paths.
    const double* in[] = {input.data()};
    double* out[] = {output.data()};
    allocations = 0;
    countAllocations = true;
    rig.processor.process(rig.ec, rig.cc, in, out, 1, nullptr, 0, int(input.size()), rig.parameters);
    rig.processor.process(rig.ec, rig.cc, nullptr, out, 1, nullptr, 0, 64, rig.parameters);
    rig.processor.process(rig.ec, rig.cc, nullptr, nullptr, 1, nullptr, 0, 0, rig.parameters);
    countAllocations = false;
    CHECK(allocations == 0);
    for (double sample : output) CHECK(std::isfinite(sample));
    // A staged latency-affecting mode change is observable, never silently prepared on audio.
    set(rig.parameters, "quality.phase", 1);
    CHECK(rig.processor.needsReprepare(rig.parameters));
    CHECK(rig.processor.latencySamples() == latency);
    rig.processor.release();
    std::fill(output.begin(), output.end(), 1.0);
    rig.processor.process(rig.ec, rig.cc, in, out, 1, nullptr, 0, int(input.size()), rig.parameters);
    CHECK(std::all_of(output.begin(), output.end(), [](double x) { return x == 0; }));
}

void liteEquivalenceAndAudibleProcessing() {
    Rig lite, full;
    set(lite.parameters, "lite.sag", 0.6);
    set(lite.parameters, "lite.thermalAge", 0.4);
    full.parameters = runtime::mapLiteParameters(lite.parameters);
    CHECK(lite.prepare(true));
    CHECK(full.prepare());
    std::vector<double> input(2048);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = 0.1 * std::sin(double(i) * 0.09);
    // LITE does not expose MIDI key tracking. Ignored notes must not even change
    // its control chunk boundaries (and hence its seeded modulation waveform).
    const control::MidiEvent ignoredNote{37, 0x90, 60, 100};
    lite.cc.midi = {&ignoredNote, 1};
    auto liteOut = lite.render(input);
    auto fullOut = full.render(input);
    double difference = 0, activity = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        CHECK(std::isfinite(liteOut[i]) && std::fabs(liteOut[i]) <= 4.0);
        CHECK(std::fabs(liteOut[i] - fullOut[i]) <= 1e-12);
        difference = std::max(difference, std::fabs(liteOut[i] - input[i]));
        activity = std::max(activity, std::fabs(liteOut[i]));
    }
    CHECK(activity > 1e-6);
    CHECK(difference > 1e-5);  // actual effect path, not the previous pass-through
    CHECK(lite.processor.latencySamples() == full.processor.latencySamples());
    engine::TelemetryFrame liteFrame, fullFrame;
    CHECK(lite.processor.readTelemetry(liteFrame));
    CHECK(full.processor.readTelemetry(fullFrame));
    CHECK(liteFrame.generation == fullFrame.generation);  // ignored MIDI creates no extra chunks
}

void midiChunkingAndZeroFrames() {
    Rig rig, split;
    set(rig.parameters, "key.enabled", 1);
    set(rig.parameters, "key.source", 1);
    set(rig.parameters, "quality.mode", 0);
    split.parameters = rig.parameters;
    CHECK(rig.prepare());
    CHECK(split.prepare());
    const control::MidiEvent event{100, 0x90, 60, 100};
    rig.cc.midi = std::span<const control::MidiEvent>(&event, 1);
    rig.processor.process(rig.ec, rig.cc, nullptr, nullptr, 1, nullptr, 0, 0, rig.parameters);
    CHECK(rig.processor.controlStatus().keyState != control::KeyState::NoteActive);
    std::vector<double> input(128);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = 0.08 * std::sin(double(i) * 0.13);
    const auto whole = rig.render(input);
    // The same note supplied as a separate host block at sample 100 must have
    // identical output; this catches applying a late event at an earlier chunk.
    const std::vector<double> prefix(input.begin(), input.begin() + 100);
    const std::vector<double> suffix(input.begin() + 100, input.end());
    auto explicitSplit = split.render(prefix);
    const control::MidiEvent startEvent{0, 0x90, 60, 100};
    split.cc.midi = std::span<const control::MidiEvent>(&startEvent, 1);
    const auto tail = split.render(suffix);
    explicitSplit.insert(explicitSplit.end(), tail.begin(), tail.end());
    for (std::size_t i = 0; i < whole.size(); ++i) CHECK(std::fabs(whole[i] - explicitSplit[i]) < 1e-12);
    CHECK(rig.processor.controlStatus().keyState == control::KeyState::NoteActive);
    CHECK(!rig.processor.controlStatus().blockClamped);
    const control::MidiEvent off{0, 0x80, 60, 0};
    rig.cc.midi = {&off, 1};
    allocations = 0;
    countAllocations = true;
    rig.processor.process(rig.ec, rig.cc, nullptr, nullptr, 1, nullptr, 0, 0, rig.parameters);
    countAllocations = false;
    CHECK(allocations == 0);
    CHECK(rig.processor.controlStatus().keyState == control::KeyState::NoteHold);
    rig.cc.midi = {&startEvent, 1};
    rig.processor.process(rig.ec, rig.cc, nullptr, nullptr, 1, nullptr, 0, 0, rig.parameters);
    CHECK(rig.processor.controlStatus().keyState == control::KeyState::NoteActive);
    std::array<control::MidiEvent, runtime::Processor::kMaxMidiEvents + 1> events{};
    for (auto& note : events) note = {0, 0x90, 60, 100};
    events.back() = {20, 0x80, 60, 0};  // note-off falls outside the accepted prefix
    rig.cc.midi = events;
    (void)rig.render(input);
    CHECK(rig.processor.midiOverflowed());
    CHECK(rig.processor.controlStatus().keyState != control::KeyState::NoteActive);
    rig.processor.process(rig.ec, rig.cc, nullptr, nullptr, 1, nullptr, 0, 0, rig.parameters);
    CHECK(rig.processor.midiOverflowed());
    CHECK(rig.processor.controlStatus().keyState != control::KeyState::NoteActive);
    std::string error;
    rig.config.maxBlock = 0;
    CHECK(!rig.processor.prepare(rig.config, rig.parameters, false, error));
    CHECK(!rig.processor.isPrepared());
}

void seekInvalidatesPitchWithoutResettingWorkerFromAudio() {
    Rig rig;
    set(rig.parameters, "key.enabled", 1);
    set(rig.parameters, "key.source", 2);  // Audio pitch detector
    CHECK(rig.prepare());
    std::vector<double> tone(12000);
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.3 * std::sin(2.0 * 3.141592653589793 * 220.0 * double(i) / 48000.0);
    (void)rig.render(tone);
    rig.processor.runPitchWorker();
    (void)rig.render(std::vector<double>(64, 0.0));
    CHECK(rig.processor.controlStatus().pitchState == control::PitchState::Acquired);

    // Sample-zero host jump is sufficient; neither audio nor worker reset
    // may be performed on the callback thread.
    ++rig.ec.epoch;
    allocations = 0;
    countAllocations = true;
    rig.processor.process(rig.ec, rig.cc, nullptr, nullptr, 1, nullptr, 0, 0, rig.parameters);
    countAllocations = false;
    CHECK(allocations == 0);
    CHECK(rig.processor.controlStatus().pitchState == control::PitchState::None);
    (void)rig.render(tone);
    rig.processor.runPitchWorker();
    (void)rig.render(std::vector<double>(64, 0.0));
    CHECK(rig.processor.controlStatus().pitchState == control::PitchState::Acquired);
}

void pendingLatencyModePreservesActiveControl() {
    Rig pending, active;
    set(pending.parameters, "pitchDrift.enabled", 1);
    set(pending.parameters, "macro.drift", 1);
    set(pending.parameters, "quality.mode", 0);
    active.parameters = pending.parameters;
    CHECK(pending.prepare());
    CHECK(active.prepare());
    std::vector<double> input(1024);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = 0.1 * std::sin(double(i) * 0.09);
    (void)pending.render(input);
    (void)active.render(input);
    const int latency = pending.processor.latencySamples();
    set(pending.parameters, "quality.zeroLatency", 1);
    set(pending.parameters, "pitchDrift.enabled", 0);
    const auto actual = pending.render(input);
    const auto expected = active.render(input);
    double difference = 0;
    for (std::size_t i = 0; i < input.size(); ++i)
        difference = std::max(difference, std::fabs(actual[i] - expected[i]));
    CHECK(difference < 1e-12);
    CHECK(pending.processor.needsReprepare(pending.parameters));
    CHECK(pending.processor.latencySamples() == latency);
    engine::TelemetryFrame frame;
    CHECK(pending.processor.readTelemetry(frame));
    CHECK(frame.activePitchDrift && !frame.activeZeroLatency);
    CHECK(!frame.pendingPitchDrift && frame.pendingZeroLatency && frame.reprepareRequired);
    CHECK(pending.prepare());
    CHECK(pending.processor.latencySamples() == 0);
    CHECK(!pending.processor.needsReprepare(pending.parameters));
}
}

void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (void* ptr = std::malloc(size == 0 ? 1 : size)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

int main() {
    parameterBoundaries();
    dryLatencyAndLargeBlocks();
    liteEquivalenceAndAudibleProcessing();
    midiChunkingAndZeroFrames();
    pendingLatencyModePreservesActiveControl();
    seekInvalidatesPitchWithoutResettingWorkerFromAudio();
    std::printf("runtime_processor: %s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
}
