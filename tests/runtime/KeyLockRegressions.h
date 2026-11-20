// T12 runtime regressions. Synthetic fixtures only; no hardware or host claim.
// Self-contained so the runtime test owner can include this header and add
// `failures += cd::tests::key_lock::run();` without sharing its local harness.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include "control/ScaleQuantizer.h"
#include "runtime/Processor.h"

namespace cd::tests::key_lock {

inline void set(core::ParamValues& values, const char* id, double value) {
    values[core::indexOf(id)] = value;
}

struct Case {
    const char* name;
    double sampleRate, baseHz;
    int source, midiNote, scaleIndex, root;
    uint16_t customMask;
};

inline int runCase(const Case& test) {
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) {
            ++failures;
            std::fprintf(stderr, "FAIL key lock %s: %s\n", test.name, message);
        }
    };

    // Independent finite enumeration of every admissible note at A4=440 Hz.
    // Registry limits govern the base knob; the resulting cutoff may be lower.
    const double effectiveBase = std::min(test.baseHz, 0.45 * test.sampleRate);
    const double lowHz = effectiveBase * 0.25;
    const double highHz = std::min(effectiveBase * 4.0, 0.45 * test.sampleRate);
    const double target = test.source ? double(test.midiNote) : control::scale::hzToMidi(effectiveBase);
    const uint16_t mask = control::scale::maskFor(test.scaleIndex, test.customMask);
    int selected = 0;
    double distance = std::numeric_limits<double>::infinity();
    for (int note = -128; note <= 256; ++note) {
        const double hz = control::scale::midiToHz(note);
        if (hz < lowHz || hz > highHz || !control::scale::isAllowed(mask, test.root, note)) continue;
        const double d = std::fabs(target - note);
        if (d < distance) { distance = d; selected = note; }
    }
    check(std::isfinite(distance), "filter domain contains a scale note");
    const double expectedHz = control::scale::midiToHz(selected);

    constexpr int block = 64;
    engine::EngineConfig config;
    config.channels = 1;
    config.maxBlock = block;
    config.sampleRate = test.sampleRate;
    auto parameters = core::defaultValues();
    set(parameters, "slot.a.filter.lpfEnabled", 1);
    set(parameters, "slot.a.filter.lpfFrequency", test.baseHz);
    set(parameters, "key.enabled", 1);
    set(parameters, "key.source", test.source);
    set(parameters, "key.scale", test.scaleIndex);
    set(parameters, "key.root", test.root);
    set(parameters, "key.mask", test.customMask);
    set(parameters, "macro.drift", 0);
    set(parameters, "tempo.depth", 0);
    set(parameters, "output.softClip", 0);
    set(parameters, "hpf.enabled", 0);
    runtime::Processor actual;
    std::string error;
    const bool prepared = actual.prepare(config, parameters, false, error);
    check(prepared, "full runtime prepares with valid LPF parameters");
    if (!prepared) return failures;

    // The independent reference keeps its base under the sample-rate ceiling
    // and supplies a known, in-range modulation directly to the real engine.
    // This catches a controller-only repair if the engine still clips the raw
    // high base after multiplying instead of admitting its base first.
    auto referenceParameters = parameters;
    set(referenceParameters, "key.enabled", 0);
    const double referenceBase = std::clamp(expectedHz, 2000.0, 20000.0);
    set(referenceParameters, "slot.a.filter.lpfFrequency", referenceBase);
    core::ModulationBlock referenceModulation;
    referenceModulation.slot[0].creativeCutoffOct = std::log2(expectedHz / referenceBase);
    engine::CircuitEngine reference;
    const bool referencePrepared = reference.prepare(config, referenceParameters, error);
    check(referencePrepared, "reference engine prepares at the selected cutoff");
    if (!referencePrepared) return failures;

    std::array<double, block> input{}, actualOutput{}, expectedOutput{};
    const double* in[] = {input.data()};
    double* actualOut[] = {actualOutput.data()};
    double* expectedOut[] = {expectedOutput.data()};
    const control::MidiEvent on{0, 0x90, uint8_t(test.midiNote), 100};
    engine::ProcessContext ec;
    ec.sampleRate = test.sampleRate;
    control::ControlContext cc;
    const int totalFrames = int(test.sampleRate * 0.25);
    const int settledFrame = int(test.sampleRate * 0.20);
    double peakError = 0.0, referencePeak = 0.0;
    for (int offset = 0; offset < totalFrames; offset += block) {
        const int frames = std::min(block, totalFrames - offset);
        ec.sampleStart = offset;
        ec.frames = uint32_t(frames);
        cc.sampleStart = uint64_t(offset);
        cc.midi = offset == 0 && test.source ? std::span<const control::MidiEvent>(&on, 1)
                                            : std::span<const control::MidiEvent>{};
        for (int i = 0; i < frames; ++i) {
            const double time = double(offset + i) / test.sampleRate;
            input[std::size_t(i)] = 0.005 * std::sin(2.0 * std::numbers::pi * std::min(0.7 * expectedHz, 0.3 * test.sampleRate) * time)
                                + 0.005 * std::sin(2.0 * std::numbers::pi * std::min(1.1 * expectedHz, 0.4 * test.sampleRate) * time);
        }
        actual.process(ec, cc, in, actualOut, 1, nullptr, 0, frames, parameters);
        reference.process(ec, in, expectedOut, 1, nullptr, 0, frames, referenceParameters, referenceModulation);
        if (offset >= settledFrame) {
            for (int i = 0; i < frames; ++i) {
                const double got = actualOutput[std::size_t(i)], expected = expectedOutput[std::size_t(i)];
                if (!std::isfinite(got) || !std::isfinite(expected)) {
                    peakError = std::numeric_limits<double>::infinity();
                    continue;
                }
                peakError = std::max(peakError, std::fabs(got - expected));
                referencePeak = std::max(referencePeak, std::fabs(expected));
            }
        }
    }

    // This is the engine's accepted modulation, after its ±2-octave guard.
    // The audio comparison below independently verifies its application.
    const auto targets = actual.effectiveTargets(0);
    const double finalHz = effectiveBase * std::exp2(targets.creativeCutoffOct);
    check(targets.creativeCutoffOct >= -2.0 && targets.creativeCutoffOct <= 2.0, "engine retains its modulation safety bounds");
    check(finalHz >= lowHz - 1e-8 && finalHz <= highHz + 1e-8, "settled engine cutoff is in admitted filter octaves");
    check(std::fabs(1200.0 * std::log2(finalHz / expectedHz)) < 0.01, "settled engine cutoff matches nearest allowed note within T12 0.01 cents");
    check(control::scale::isAllowed(mask, test.root, selected), "selected note belongs to the requested scale");
    check(actual.controlStatus().scaleLockActive, "runtime reports active scale lock");
    check(referencePeak > 1e-7, "rendered reference has a measurable nonzero signal");
    check(peakError < 1e-9, "actual filter audio matches the independent selected-cutoff engine");
    return failures;
}

inline int run() {
    int failures = 0;
    const Case cases[] = {
        {"low MIDI / high base", 48000, 18000, 1, 60, 0, 0, 4095},
        {"high MIDI / low base", 48000, 2000, 1, 127, 0, 0, 4095},
        {"effective cutoff below knob minimum", 48000, 2000, 1, 60, 0, 0, 4095},
        {"manual / low-Fs ceiling", 8000, 20000, 0, 0, 0, 0, 4095},
        {"high MIDI / low-Fs ceiling", 8000, 20000, 1, 127, 0, 0, 4095},
        {"low MIDI / low-Fs high base", 8000, 18000, 1, 60, 0, 0, 4095},
        {"restrictive C-only scale", 48000, 18000, 1, 60, 5, 0, 1},
        {"restrictive C# / low-Fs ceiling", 8000, 20000, 1, 127, 5, 1, 1}
    };
    for (const auto& test : cases) failures += runCase(test);
    return failures;
}

}  // namespace cd::tests::key_lock
