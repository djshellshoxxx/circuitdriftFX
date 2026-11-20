// Included inside the runtime test namespace after Rig. Synthetic audio only.
#pragma once

void configureMidiFilteringRig(Rig& rig, control::KeySource source) {
    rig.config.seed = 0x1020304050607080ull;
    set(rig.parameters, "quality.mode", 0);
    set(rig.parameters, "slot.a.filter.lpfEnabled", 1);
    set(rig.parameters, "slot.a.filter.lpfFrequency", 2400);
    set(rig.parameters, "slot.b.filter.lpfEnabled", 1);
    set(rig.parameters, "slot.b.filter.lpfFrequency", 3100);
    set(rig.parameters, "macro.drift", 0.8);
    set(rig.parameters, "tempo.enabled", 1);
    set(rig.parameters, "tempo.depth", 0.65);
    set(rig.parameters, "tempo.division", 0);
    set(rig.parameters, "key.enabled", 1);
    set(rig.parameters, "key.source", int(source));
    rig.cc.bpm = 137.0;
    rig.cc.ppq = 3.1875;
    rig.cc.timeSigNum = 7;
    rig.cc.timeSigDen = 8;
    rig.cc.playing = true;
}

std::vector<double> midiFilteringAudio(std::size_t frames) {
    std::vector<double> input(frames);
    for (std::size_t i = 0; i < frames; ++i)
        input[i] = 0.1 * std::sin(2.0 * 3.141592653589793 * 220.0 * double(i) / 48000.0) +
                   0.03 * std::sin(2.0 * 3.141592653589793 * 1800.0 * double(i) / 48000.0);
    return input;
}

void checkMidiFilteringResult(Rig& actual, Rig& expected,
                              const std::vector<double>& actualOut,
                              const std::vector<double>& expectedOut) {
    CHECK(actualOut.size() == expectedOut.size());
    double peakDifference = 0.0, activity = 0.0;
    bool finite = true;
    for (std::size_t i = 0; i < std::min(actualOut.size(), expectedOut.size()); ++i) {
        finite = finite && std::isfinite(actualOut[i]) && std::isfinite(expectedOut[i]);
        peakDifference = std::max(peakDifference, std::fabs(actualOut[i] - expectedOut[i]));
        activity = std::max(activity, std::fabs(expectedOut[i]));
    }
    CHECK(finite);
    CHECK(activity > 1e-6);
    CHECK(peakDifference <= 1e-12);
    const auto& a = actual.processor.controlStatus();
    const auto& b = expected.processor.controlStatus();
    CHECK(a.keyState == b.keyState && a.scaleLockActive == b.scaleLockActive);
    CHECK(a.pitchDriftHeld == b.pitchDriftHeld);
    CHECK(std::fabs(a.referenceNote - b.referenceNote) <= 1e-12);
    CHECK(a.tempoActive == b.tempoActive && a.tempoFallback == b.tempoFallback);
    CHECK(std::fabs(a.tempoPhase - b.tempoPhase) <= 1e-12);
    CHECK(a.pitchState == b.pitchState && a.pitchHz == b.pitchHz && a.pitchConfidence == b.pitchConfidence);
    CHECK(!a.blockClamped && !b.blockClamped);
    for (int slot = 0; slot < 2; ++slot) {
        const auto at = actual.processor.effectiveTargets(slot);
        const auto bt = expected.processor.effectiveTargets(slot);
        CHECK(std::fabs(at.creativeCutoffOct - bt.creativeCutoffOct) <= 1e-12);
        CHECK(std::fabs(at.creativeDelayMs - bt.creativeDelayMs) <= 1e-12);
        CHECK(std::fabs(at.recoveryTauS - bt.recoveryTauS) <= 1e-12);
    }
    engine::TelemetryFrame actualFrame, expectedFrame;
    CHECK(actual.processor.readTelemetry(actualFrame));
    CHECK(expected.processor.readTelemetry(expectedFrame));
    CHECK(actualFrame.generation == expectedFrame.generation);
    CHECK(actualFrame.sampleIndex == expectedFrame.sampleIndex);
}

void renderMidiFilteringPair(Rig& actual, Rig& expected, const std::vector<double>& input) {
    std::vector<double> actualOut(input.size()), expectedOut(input.size());
    const double* in[] = {input.data()};
    double* actualOutput[] = {actualOut.data()};
    double* expectedOutput[] = {expectedOut.data()};
    allocations = 0;
    countAllocations = true;
    actual.processor.process(actual.ec, actual.cc, in, actualOutput, 1, nullptr, 0,
                             int(input.size()), actual.parameters);
    expected.processor.process(expected.ec, expected.cc, in, expectedOutput, 1, nullptr, 0,
                               int(input.size()), expected.parameters);
    countAllocations = false;
    CHECK(allocations == 0);
    for (Rig* rig : {&actual, &expected}) {
        rig->ec.sampleStart += int64_t(input.size());
        rig->cc.sampleStart += uint64_t(input.size());
        if (rig->cc.ppq && rig->cc.playing)
            *rig->cc.ppq += double(input.size()) * rig->cc.bpm.value_or(120.0) / (60.0 * rig->config.sampleRate);
    }
    checkMidiFilteringResult(actual, expected, actualOut, expectedOut);
}

void ignoredMidiSourcesPreserveProcessing() {
    const auto input = midiFilteringAudio(256);
    const control::MidiEvent ignoredNote{37, 0x93, 60, 100};
    std::array<control::MidiEvent, runtime::Processor::kMaxMidiEvents + 1> overflow{};
    overflow.fill(ignoredNote);
    // The first two cases are Full with an audible LPF and nonzero drift/tempo.
    // The third also pins LITE's existing raw received-event overflow diagnostic.
    for (int mode = 0; mode < 3; ++mode) {
        Rig actual, expected;
        const bool lite = mode == 2;
        configureMidiFilteringRig(actual, mode == 1 ? control::KeySource::Audio : control::KeySource::Manual);
        configureMidiFilteringRig(expected, mode == 1 ? control::KeySource::Audio : control::KeySource::Manual);
        if (lite) expected.parameters = runtime::mapLiteParameters(actual.parameters);
        CHECK(actual.prepare(lite));
        CHECK(expected.prepare());
        renderMidiFilteringPair(actual, expected, midiFilteringAudio(512));
        actual.cc.midi = {&ignoredNote, 1};
        renderMidiFilteringPair(actual, expected, input);
        CHECK(!actual.processor.midiOverflowed());
        actual.cc.midi = overflow;
        renderMidiFilteringPair(actual, expected, input);
        CHECK(actual.processor.midiOverflowed());
        CHECK(!expected.processor.midiOverflowed());
        actual.cc.midi = {};
        renderMidiFilteringPair(actual, expected, input);
        CHECK(!actual.processor.midiOverflowed());
    }
}

void irrelevantMidiMessagesPreserveProcessing() {
    Rig actual, expected;
    configureMidiFilteringRig(actual, control::KeySource::Midi);
    configureMidiFilteringRig(expected, control::KeySource::Midi);
    CHECK(actual.prepare());
    CHECK(expected.prepare());
    const auto input = midiFilteringAudio(256);
    const control::MidiEvent note{0, 0x92, 60, 100};
    actual.cc.midi = expected.cc.midi = {&note, 1};
    renderMidiFilteringPair(actual, expected, midiFilteringAudio(512));
    const std::array<control::MidiEvent, 9> ignored{{
        {37, 0xb2, 1, 127}, {37, 0xc2, 10, 0}, {59, 0xa2, 60, 100},
        {73, 0xd2, 100, 0}, {91, 0xe2, 0, 64}, {113, 0xf8, 0, 0},
        {129, 0xb2, 121, 0}, {151, 0xb2, 7, 100}, {171, 0xf2, 0, 0}
    }};
    actual.cc.midi = ignored;
    expected.cc.midi = {};
    renderMidiFilteringPair(actual, expected, input);
    CHECK(actual.processor.controlStatus().keyState == control::KeyState::NoteActive);
    CHECK(actual.processor.controlStatus().referenceNote == 60.0);
    CHECK(!actual.processor.midiOverflowed());

    // Unhandled CCs call evaluate(true) in KeyTracker, but are also no-ops while
    // the last note is held or returning. Keep real sample-zero releases.
    for (const auto release : {control::MidiEvent{0, 0x92, 60, 0}, control::MidiEvent{0, 0xb2, 123, 0}}) {
        std::array<control::MidiEvent, ignored.size() + 1> mixed{};
        mixed[0] = release;
        std::copy(ignored.begin(), ignored.end(), mixed.begin() + 1);
        actual.cc.midi = mixed;
        expected.cc.midi = {&release, 1};
        renderMidiFilteringPair(actual, expected, input);
        CHECK(actual.processor.controlStatus().keyState ==
              (release.status == 0x92 ? control::KeyState::NoteHold : control::KeyState::Returning));
    }
}

void midiFilteringKeepsReceivedAdmissionLimit() {
    Rig actual, expected;
    configureMidiFilteringRig(actual, control::KeySource::Midi);
    configureMidiFilteringRig(expected, control::KeySource::Midi);
    CHECK(actual.prepare());
    CHECK(expected.prepare());
    const auto input = midiFilteringAudio(256);
    const control::MidiEvent note{0, 0x90, 60, 100};
    actual.cc.midi = expected.cc.midi = {&note, 1};
    renderMidiFilteringPair(actual, expected, input);

    std::array<control::MidiEvent, runtime::Processor::kMaxMidiEvents + 1> events{};
    events.fill({37, 0xb0, 1, 127});
    events.back() = {37, 0x90, 72, 100};  // meaningful, but beyond the admitted raw prefix
    actual.cc.midi = {events.data(), runtime::Processor::kMaxMidiEvents};
    expected.cc.midi = {};
    renderMidiFilteringPair(actual, expected, input);
    CHECK(!actual.processor.midiOverflowed());
    CHECK(actual.processor.controlStatus().keyState == control::KeyState::NoteActive);
    CHECK(actual.processor.controlStatus().referenceNote == 60.0);

    // Overflow is based on received events, before filtering. Even an entirely
    // irrelevant admitted prefix must release the note held from an earlier call.
    const control::MidiEvent release{37, 0xb0, 123, 0};
    actual.cc.midi = events;
    expected.cc.midi = {&release, 1};
    renderMidiFilteringPair(actual, expected, input);
    CHECK(actual.processor.midiOverflowed());
    CHECK(!expected.processor.midiOverflowed());
    CHECK(actual.processor.controlStatus().keyState == control::KeyState::Returning);
}

void disabledScaleLockRetainsMidiTiming() {
    Rig actual, split;
    configureMidiFilteringRig(actual, control::KeySource::Midi);
    configureMidiFilteringRig(split, control::KeySource::Midi);
    set(actual.parameters, "key.enabled", 0);
    set(split.parameters, "key.enabled", 0);
    // Free-running transport avoids introducing a host PPQ discontinuity in the
    // explicit split. Both rigs still have identical BPM and nonzero tempo depth.
    actual.cc.ppq.reset();
    split.cc.ppq.reset();
    CHECK(actual.prepare());
    CHECK(split.prepare());
    const auto input = midiFilteringAudio(256);
    const control::MidiEvent note{37, 0x95, 60, 100};
    actual.cc.midi = {&note, 1};
    actual.processor.process(actual.ec, actual.cc, nullptr, nullptr, 1, nullptr, 0, 0, actual.parameters);
    split.processor.process(split.ec, split.cc, nullptr, nullptr, 1, nullptr, 0, 0, split.parameters);
    CHECK(actual.processor.controlStatus().referenceNote == split.processor.controlStatus().referenceNote);
    const auto whole = actual.render(input);
    auto explicitSplit = split.render(std::vector<double>(input.begin(), input.begin() + 37));
    const control::MidiEvent atStart{0, 0x95, 60, 100};
    split.cc.midi = {&atStart, 1};
    const auto tail = split.render(std::vector<double>(input.begin() + 37, input.end()));
    explicitSplit.insert(explicitSplit.end(), tail.begin(), tail.end());
    checkMidiFilteringResult(actual, split, whole, explicitSplit);
    CHECK(actual.processor.controlStatus().keyState == control::KeyState::Off);
    CHECK(actual.processor.controlStatus().referenceNote == 60.0);

    actual.cc.midi = split.cc.midi = {};
    set(actual.parameters, "key.enabled", 1);
    set(split.parameters, "key.enabled", 1);
    allocations = 0;
    countAllocations = true;
    actual.processor.process(actual.ec, actual.cc, nullptr, nullptr, 1, nullptr, 0, 0, actual.parameters);
    split.processor.process(split.ec, split.cc, nullptr, nullptr, 1, nullptr, 0, 0, split.parameters);
    countAllocations = false;
    CHECK(allocations == 0);
    CHECK(actual.processor.controlStatus().keyState == control::KeyState::NoteActive);
    CHECK(actual.processor.controlStatus().referenceNote == 60.0);
    renderMidiFilteringPair(actual, split, input);
}
