// Synthetic aggregate control continuation. The oracle is an independently
// advanced live processor receiving the same future calls, not a second restore.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include "TestHarness.h"
#include "control/ControlProcessor.h"

// Scope: ordinary and nothrow C++ new/new[]. No malloc/aligned-allocation,
// locking, I/O or scheduler instrumentation is claimed.
namespace checkpoint_allocations {
std::atomic<bool> observing{false};
std::atomic<std::size_t> count{0};
void* allocate(std::size_t n) {
    if (observing.load(std::memory_order_relaxed)) count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t n) { return checkpoint_allocations::allocate(n); }
void* operator new[](std::size_t n) { return checkpoint_allocations::allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return checkpoint_allocations::allocate(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return checkpoint_allocations::allocate(n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
using namespace cd;
using namespace cd::control;
using Result = ControlSnapshotResult;
constexpr double kFs = 48000.0;
constexpr int kBlock = 257;
constexpr uint64_t kSeed = 0x192837465ULL;
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
static_assert(std::is_trivially_copyable_v<ControlSnapshot>);
static_assert(noexcept(std::declval<const ControlProcessor&>().captureSnapshot(std::declval<ControlSnapshot&>())));
static_assert(noexcept(std::declval<const ControlProcessor&>().canRestoreSnapshot(std::declval<const ControlSnapshot&>())));
static_assert(noexcept(std::declval<ControlProcessor&>().restoreSnapshot(std::declval<const ControlSnapshot&>())));

// Compare semantic fields, never padding or representation bytes.
bool same(const CreativeDrift::State& a, const CreativeDrift::State& b) {
    if (a.sampleRate != b.sampleRate || a.samples != b.samples || a.ticks != b.ticks ||
        a.q != b.q || a.delayUnitMs != b.delayUnitMs) return false;
    for (int i = 0; i < 3; ++i) {
        if (a.u[i] != b.u[i] || a.rng[i].spare != b.rng[i].spare || a.rng[i].hasSpare != b.rng[i].hasSpare) return false;
        for (int j = 0; j < 4; ++j) if (a.rng[i].s[j] != b.rng[i].s[j]) return false;
    }
    return true;
}
bool same(const ResonanceQuantizer::State& a, const ResonanceQuantizer::State& b) {
    return a.mask == b.mask && a.root == b.root && a.haveNote == b.haveNote && a.haveOutput == b.haveOutput &&
        a.note == b.note && a.glideNote == b.glideNote && a.current == b.current && a.start == b.start &&
        a.glideRemaining == b.glideRemaining;
}
bool same(const TempoOutput& a, const TempoOutput& b) {
    return a.recoveryTauS == b.recoveryTauS && a.cutoffOct == b.cutoffOct && a.fallback == b.fallback &&
        a.bpmUsed == b.bpmUsed && a.phase == b.phase && a.beatsPerCycle == b.beatsPerCycle;
}
bool same(const TempoBreathing::State& a, const TempoBreathing::State& b) {
    return a.sampleRate == b.sampleRate && a.phase == b.phase && a.phaseError == b.phaseError &&
        a.recoveryTau == b.recoveryTau && a.wasEnabled == b.wasEnabled && a.havePpq == b.havePpq &&
        a.lastPpqEnd == b.lastPpqEnd && a.lastBeatsPerCycle == b.lastBeatsPerCycle && same(a.output, b.output);
}
bool same(const KeyTracker::State& a, const KeyTracker::State& b) {
    if (a.sampleRate != b.sampleRate || a.count != b.count || a.pedal != b.pedal || a.state != b.state ||
        a.notePitch != b.notePitch || a.weight != b.weight || a.holdLeft != b.holdLeft || a.returnLeft != b.returnLeft) return false;
    for (std::size_t i = 0; i < a.notes.size(); ++i)
        if (a.notes[i].note != b.notes[i].note || a.notes[i].held != b.notes[i].held ||
            a.notes[i].sustained != b.notes[i].sustained) return false;
    return true;
}
bool same(const TransientDetector::State& a, const TransientDetector::State& b) {
    return a.sampleRate == b.sampleRate && a.fast == b.fast && a.slow == b.slow && a.held == b.held && a.holdCount == b.holdCount;
}
bool same(const SidechainLoad::State& a, const SidechainLoad::State& b) {
    if (a.sampleRate != b.sampleRate || a.hpfHz != b.hpfHz || a.meanSquare != b.meanSquare || a.envelope != b.envelope ||
        a.budgetAmps != b.budgetAmps || a.lastRatio != b.lastRatio) return false;
    for (int i = 0; i < 2; ++i) if (a.z1[i] != b.z1[i] || a.z2[i] != b.z2[i]) return false;
    return true;
}
bool same(const ControlStatus& a, const ControlStatus& b) {
    return a.tempoActive == b.tempoActive && a.tempoFallback == b.tempoFallback && a.tempoPhase == b.tempoPhase &&
        a.keyState == b.keyState && a.scaleLockActive == b.scaleLockActive && a.pitchDriftHeld == b.pitchDriftHeld &&
        a.referenceNote == b.referenceNote && a.pitchState == b.pitchState && a.pitchHz == b.pitchHz &&
        a.pitchConfidence == b.pitchConfidence && a.harmonicAvailable[0] == b.harmonicAvailable[0] &&
        a.harmonicAvailable[1] == b.harmonicAvailable[1] && a.sidechainConnected == b.sidechainConnected &&
        a.sidechainMissing == b.sidechainMissing && a.blockClamped == b.blockClamped;
}
bool same(const ControlSnapshot& a, const ControlSnapshot& b) {
    if (a.version != b.version || a.sampleRate != b.sampleRate || a.maxBlock != b.maxBlock || a.instanceSeed != b.instanceSeed ||
        a.sidechainBudgetAmps != b.sidechainBudgetAmps || a.a4Hz != b.a4Hz || a.keySource != b.keySource ||
        a.audioPitchActive != b.audioPitchActive || !same(a.status, b.status) || !same(a.tempo, b.tempo) ||
        !same(a.key, b.key) || !same(a.transient, b.transient) || !same(a.sidechain, b.sidechain)) return false;
    for (std::size_t i = 0; i < a.slots.size(); ++i) {
        const auto& x = a.slots[i]; const auto& y = b.slots[i];
        if (!same(x.drift, y.drift) || !same(x.quantizer, y.quantizer) || x.lockWeight != y.lockWeight ||
            x.delayHold != y.delayHold || x.cutoffOct != y.cutoffOct || x.delayMs != y.delayMs ||
            x.lastQuantizedOffset != y.lastQuantizedOffset) return false;
    }
    return true;
}
bool same(const core::SlotModulation& a, const core::SlotModulation& b) {
    return a.ageYearsMin == b.ageYearsMin && a.toleranceSigmaAdd == b.toleranceSigmaAdd && a.leakageAdd == b.leakageAdd &&
        a.hfShelfDb == b.hfShelfDb && a.ageNoiseDbfs == b.ageNoiseDbfs && a.reservoirCapDivisor == b.reservoirCapDivisor &&
        a.sagMacro == b.sagMacro && a.driveOffsetDb == b.driveOffsetDb && a.biasOffset == b.biasOffset &&
        a.fluxDriveFactor == b.fluxDriveFactor && a.thermalSensitivityScale == b.thermalSensitivityScale &&
        a.creativeCutoffOct == b.creativeCutoffOct && a.creativeDelayMs == b.creativeDelayMs &&
        a.creativeDelayActive == b.creativeDelayActive && a.recoveryTauS == b.recoveryTauS && a.sidechainMode == b.sidechainMode;
}
ControlSnapshot capture(const ControlProcessor& p, const char* label) {
    ControlSnapshot s;
    CHECK(p.captureSnapshot(s) == Result::Ok, "capture succeeds: %s", label);
    return s;
}
core::ParamValues parameters() {
    auto p = core::defaultValues();
    p[core::indexOf("macro.age")] = 0.37; p[core::indexOf("macro.sag")] = 0.61;
    p[core::indexOf("macro.drift")] = 0.83; p[core::indexOf("macro.warmth")] = 0.29;
    p[core::indexOf("analogNoise.enabled")] = 1; p[core::indexOf("pitchDrift.enabled")] = 1;
    p[core::indexOf("quality.zeroLatency")] = 0;
    p[core::indexOf("key.enabled")] = 1; p[core::indexOf("key.source")] = int(KeySource::Midi);
    p[core::indexOf("key.scale")] = 0; p[core::indexOf("key.root")] = 0;
    p[core::indexOf("slot.a.filter.lpfFrequency")] = 5200; p[core::indexOf("slot.b.filter.lpfFrequency")] = 9000;
    p[core::indexOf("tempo.enabled")] = 1; p[core::indexOf("tempo.depth")] = 0.71;
    p[core::indexOf("tempo.division")] = 3; p[core::indexOf("tempo.modifier")] = 0;
    p[core::indexOf("sidechain.source")] = int(SidechainSource::External);
    p[core::indexOf("sidechain.depth")] = 1.3; p[core::indexOf("sidechain.hpf")] = 88;
    p[core::indexOf("transient.preserve")] = 0.81;
    return p;
}
struct Signal {
    std::array<double, kBlock> left{}, right{}, sideL{}, sideR{};
    explicit Signal(int phase) {
        for (int i = 0; i < kBlock; ++i) {
            const double t = double(i + 113 * phase);
            left[std::size_t(i)] = i % 71 == 0 ? 0.87 : 0.003 * std::sin(t * 0.071);
            right[std::size_t(i)] = i % 89 == 0 ? -0.93 : 0.004 * std::cos(t * 0.053);
            sideL[std::size_t(i)] = 0.18 * std::sin(t * 0.117);
            sideR[std::size_t(i)] = 0.11 * std::cos(t * 0.153);
        }
    }
    void process(ControlProcessor& p, const core::ParamValues& values, const ControlContext& ctx,
                 core::ModulationBlock& out, bool connected = true, bool stereo = true) const {
        const double* main[] = {left.data(), right.data()};
        const double* ext[] = {sideL.data(), sideR.data()};
        const AudioView in{main, stereo ? 2 : 1}, sc{ext, stereo ? 2 : 1};
        p.processBlock(values, ctx, in, connected ? &sc : nullptr, out);
    }
};
void processPair(ControlProcessor& reference, ControlProcessor& actual, const core::ParamValues& values,
                 const ControlContext& ctx, int phase, const char* label, bool connected = true, bool stereo = true) {
    const Signal signal(phase); core::ModulationBlock expected, output;
    signal.process(reference, values, ctx, expected, connected, stereo);
    signal.process(actual, values, ctx, output, connected, stereo);
    CHECK(same(expected.slot[0], output.slot[0]) && same(expected.slot[1], output.slot[1]),
          "all future modulation scalars match, phase %d: %s", phase, label);
    CHECK(same(reference.status(), actual.status()), "all future status fields match, phase %d: %s", phase, label);
    CHECK(expected.externalLoadA && output.externalLoadA && expected.transientPreserve && output.transientPreserve,
          "prepared output spans exist: %s", label);
    const int frames = std::clamp(ctx.frames, 0, kBlock);
    for (int i = 0; i < frames; ++i)
        CHECK(expected.externalLoadA[i] == output.externalLoadA[i] && expected.transientPreserve[i] == output.transientPreserve[i],
              "exact future sidechain/transient sample %d, phase %d: %s", i, phase, label);
}
void warm(ControlProcessor& p, int variant = 0) {
    auto values = parameters();
    p.setSidechainBudgetA(0.031 + 0.01 * variant); p.setReferenceTuning(432 + 10 * variant);
    ControlContext ctx; ctx.frames = kBlock; ctx.bpm = 109; ctx.ppq = 3.125 + variant;
    const MidiEvent start[] = {{0, 0x90, 72, 100}, {31, 0x91, 76, 80}, {79, 0xB0, 64, 127},
                               {127, 0x81, 76, 0}, {200, 0x90, 60, 90}};
    ctx.midi = start; core::ModulationBlock output; Signal(variant).process(p, values, ctx, output);
    values[core::indexOf("key.scale")] = 1; values[core::indexOf("key.root")] = 2 + variant;
    ctx.frames = 127; ctx.midi = {}; ctx.bpm = 147; ctx.ppq = -1.75 + variant;
    Signal(7 + variant).process(p, values, ctx, output);
    const MidiEvent release[] = {{0, 0x80, 60, 0}, {13, 0x80, 72, 0}};
    ctx.frames = 31; ctx.midi = release; Signal(11 + variant).process(p, values, ctx, output);
    values[core::indexOf("sidechain.hpf")] = 233; values[core::indexOf("tempo.division")] = 0;
    values[core::indexOf("tempo.modifier")] = 1;
    ctx.frames = 0; ctx.midi = {}; ctx.bpm = 73; ctx.ppq = 99;
    Signal(17).process(p, values, ctx, output, false);
}

void future(ControlProcessor& reference, ControlProcessor& actual, const char* label, bool full = false) {
    auto values = parameters(); values[core::indexOf("sidechain.hpf")] = 233;
    ControlContext ctx; ctx.bpm = 131; ctx.ppq = -3.0;
    const MidiEvent pedalUp[] = {{0, 0xB0, 64, 0}};
    bool held = false, returning = false, manual = false;
    // The long replay lets the captured sustain release, 500 ms hold and 100 ms
    // return complete before any fresh note can overwrite the restored history.
    const int blocks = full ? 135 : 9;
    for (int block = 0; block < blocks; ++block) {
        ctx.frames = block == 0 ? 0 : block == 1 ? 1 : block == 2 ? 47 : kBlock;
        ctx.midi = block == 2 ? std::span<const MidiEvent>(pedalUp) : std::span<const MidiEvent>{};
        ctx.sampleStart = uint64_t(block) * 997;  // informational clock intentionally differs from drift
        ctx.ppq = block < 4 ? std::optional<double>(-3.0 + block * 0.125) : std::nullopt;
        ctx.bpm = block == 4 ? std::nullopt : std::optional<double>(137.0);
        processPair(reference, actual, values, ctx, block, label, true, block % 2 == 0);
        const auto a = capture(reference, label), b = capture(actual, label);
        CHECK(same(a, b), "future aggregate state matches: %s", label);
        held |= a.key.state == KeyState::NoteHold;
        returning |= a.key.state == KeyState::Returning;
        manual |= a.key.state == KeyState::ManualReference;
    }
    if (full) CHECK(held && returning && manual, "captured sustain traverses hold, return and manual: %s", label);
    for (int step = 0; step < 7; ++step) {
        ctx.frames = step == 0 ? 0 : step == 1 ? -1 : step == 6 ? kBlock + 31 : 37;
        ctx.midi = {}; ctx.ppq = 71.25 - step; ctx.bpm = 93;
        values[core::indexOf("key.source")] = step < 3 ? int(KeySource::Manual) : int(KeySource::Midi);
        values[core::indexOf("key.enabled")] = step < 2 ? 0 : 1;
        values[core::indexOf("tempo.enabled")] = step == 2 ? 0 : 1;
        values[core::indexOf("key.scale")] = step % 2 ? 5 : 1;
        values[core::indexOf("key.mask")] = 0x295; values[core::indexOf("key.root")] = step;
        values[core::indexOf("sidechain.hpf")] = step == 0 ? 23 : 177;
        values[core::indexOf("sidechain.source")] = step == 4 ? int(SidechainSource::Main) : int(SidechainSource::Blend);
        const MidiEvent notes[] = {{0, 0x90, uint8_t(81 + step), 100}, {19, 0x90, 200, 0}};
        if (step >= 3) ctx.midi = notes;
        processPair(reference, actual, values, ctx, 200 + step, label, step != 5);
        CHECK(same(capture(reference, label), capture(actual, label)), "future edit/clamped-call state matches: %s", label);
    }
}
void restoreAndReplay(ControlProcessor& reference, ControlProcessor& receiver, const char* label, bool full = false) {
    const auto captured = capture(reference, label), before = capture(receiver, label);
    CHECK(receiver.canRestoreSnapshot(captured) == Result::Ok, "compatible preflight: %s", label);
    CHECK(same(capture(receiver, label), before), "preflight leaves receiver unchanged: %s", label);
    CHECK(receiver.restoreSnapshot(captured) == Result::Ok, "compatible restore: %s", label);
    CHECK(same(capture(receiver, label), captured) && same(capture(reference, label), captured), "exact restored value: %s", label);
    future(reference, receiver, label, full);
    reference.resetRealtime(); receiver.resetRealtime();
    CHECK(same(capture(reference, label), capture(receiver, label)), "matching prepared reset origin: %s", label);
    future(reference, receiver, label);
}

void continuation() {
    for (double fs : {44100.5, kFs}) {
        ControlProcessor reference, receiver; reference.prepare(fs, kBlock, kSeed); receiver.prepare(fs, kBlock, kSeed);
        warm(receiver, 1);
        restoreAndReplay(reference, receiver, "fresh prepared snapshot");
        reference.reset(); receiver.reset(); warm(reference); warm(receiver, 1);
        auto s = capture(reference, "warm setup");
        CHECK(s.key.pedal && s.key.count == 3 && s.key.state == KeyState::NoteActive && s.tempo.phaseError != 0 &&
                  s.tempo.output.beatsPerCycle != s.tempo.lastBeatsPerCycle && s.sidechain.hpfHz == 233 &&
                  s.sidechain.meanSquare > 0 && s.transient.fast > 0 && s.slots[0].lockWeight > 0 && s.slots[0].lockWeight < 1,
              "warm fixture has sustain, tempo/cache, detector and blend histories");
        reference.setSidechainBudgetA(0.075); reference.setReferenceTuning(458);
        reference.drift(0).prepare(fs, 0xABCDEF, 1); reference.drift(0).advance(17);
        reference.drift(1).advance(93);
        s = capture(reference, "pending setters and independent drift");
        CHECK(s.sidechainBudgetAmps == 0.075 && s.sidechain.budgetAmps == 0.031 && s.a4Hz == 458 &&
                  s.slots[0].drift.samples != s.slots[1].drift.samples,
              "published settings are distinct from history and drift clocks are independent");
        restoreAndReplay(reference, receiver, "warm MIDI/tempo/detector continuation", true);
    }
    // Invalid input normalization belongs to preparation, so compatibility uses
    // its effective 48 kHz / zero-capacity identity rather than the raw arguments.
    ControlProcessor normalized, zeroPeer; normalized.prepare(kNan, -7, kSeed); zeroPeer.prepare(kFs, 0, kSeed);
    const auto zero = capture(normalized, "normalized zero-capacity preparation");
    CHECK(zero.sampleRate == kFs && zero.maxBlock == 0 && zeroPeer.restoreSnapshot(zero) == Result::Ok,
          "effective preparation identity round trips");
    for (double pending : {kNan, kInf, -1.0, 0.075, std::numeric_limits<double>::max()}) {
        ControlProcessor reference, receiver; reference.prepare(kFs, kBlock, kSeed); receiver.prepare(kFs, kBlock, kSeed);
        warm(reference); warm(receiver, 1); reference.setSidechainBudgetA(pending);
        reference.setReferenceTuning(pending == 0.075 ? std::numeric_limits<double>::denorm_min() :
                                     pending > 1 ? std::numeric_limits<double>::max() : 441.0);
        const auto s = capture(reference, "pending canonical budget");
        const double expected = std::isfinite(pending) && pending > 0 ?
            std::min(pending, std::numeric_limits<double>::max() / 8) : 0;
        CHECK(s.sidechainBudgetAmps == expected && s.sidechain.budgetAmps == 0.031,
              "capture retains previous applied budget while canonicalizing pending value");
        restoreAndReplay(reference, receiver, "pending budget/tuning normalization");
    }
}

void rejectedCandidates() {
    struct Mutation { const char* label; void (*apply)(ControlSnapshot&); Result result; };
    const Mutation cases[] = {
        {"version", [](auto& s) { ++s.version; }, Result::Incompatible},
        {"rate", [](auto& s) { s.sampleRate = 44100; }, Result::Incompatible},
        {"capacity", [](auto& s) { ++s.maxBlock; }, Result::Incompatible},
        {"reset seed", [](auto& s) { s.instanceSeed ^= 1; }, Result::Incompatible},
        {"source enum", [](auto& s) { s.keySource = -1; }, Result::InvalidState},
        {"audio selection", [](auto& s) { s.keySource = int(KeySource::Audio); }, Result::UnsupportedAudioPitch},
        {"audio active", [](auto& s) { s.audioPitchActive = true; }, Result::UnsupportedAudioPitch},
        {"pitch result", [](auto& s) { s.status.pitchState = PitchState::Acquired; }, Result::UnsupportedAudioPitch},
        {"pitch enum", [](auto& s) { s.status.pitchState = PitchState(255); }, Result::InvalidState},
        {"harmonic availability", [](auto& s) { s.status.harmonicAvailable[1] = true; }, Result::UnsupportedHarmonicMap},
        {"pending budget", [](auto& s) { s.sidechainBudgetAmps = kInf; }, Result::InvalidState},
        {"tuning", [](auto& s) { s.a4Hz = kNan; }, Result::InvalidState},
        {"status reference", [](auto& s) { s.status.referenceNote = kInf; }, Result::InvalidState},
        {"status tempo consistency", [](auto& s) { s.status.tempoPhase = 1.1; }, Result::InvalidState},
        {"status sidechain consistency", [](auto& s) { s.status.sidechainConnected = s.status.sidechainMissing = true; }, Result::InvalidState},
        {"key table bound", [](auto& s) { s.key.count = KeyTracker::kMaxNotes + 1; }, Result::InvalidState},
        {"transient count", [](auto& s) { s.transient.holdCount = std::numeric_limits<int>::max(); }, Result::InvalidState},
        {"sidechain final history", [](auto& s) { s.sidechain.z2[1] = kInf; }, Result::InvalidState},
        {"slot 1 drift clock", [](auto& s) { s.slots[1].drift.sampleRate = 44100; }, Result::InvalidState},
        {"slot 1 final RNG", [](auto& s) { s.slots[1].drift.rng[2].hasSpare = 2; }, Result::InvalidState},
        {"slot 1 quantizer duration", [](auto& s) { s.slots[1].quantizer.glideRemaining = -1; }, Result::InvalidState},
        {"slot 1 blend", [](auto& s) { s.slots[1].lockWeight = kNan; }, Result::InvalidState},
        {"slot 1 cached offset", [](auto& s) { s.slots[1].lastQuantizedOffset = 2.01; }, Result::InvalidState}
    };
    ControlProcessor donor; donor.prepare(kFs, kBlock, kSeed); warm(donor, 1); donor.drift(0).advance(771);
    donor.setSidechainBudgetA(0.091); donor.setReferenceTuning(477);
    const auto changed = capture(donor, "different valid early fields");
    for (const auto& test : cases) {
        ControlProcessor reference, actual; reference.prepare(kFs, kBlock, kSeed); actual.prepare(kFs, kBlock, kSeed);
        warm(reference); warm(actual);
        const auto before = capture(actual, test.label); auto bad = changed; test.apply(bad);
        CHECK(!same(before, changed), "candidate has distinct valid fields before its invalid field");
        CHECK(actual.canRestoreSnapshot(bad) == test.result, "preflight reason: %s", test.label);
        CHECK(same(capture(actual, test.label), before), "failed preflight is pure: %s", test.label);
        CHECK(actual.restoreSnapshot(bad) == test.result, "restore rejection reason: %s", test.label);
        CHECK(same(capture(actual, test.label), before), "entire live aggregate unchanged: %s", test.label);
        future(reference, actual, test.label);
    }
}

void expectUnsupported(ControlProcessor& p, const ControlSnapshot& good, Result expected, const char* label) {
    auto destination = good; destination.version = 77; destination.instanceSeed = 999;
    const auto sentinel = destination;
    const auto d0 = p.drift(0).save(), d1 = p.drift(1).save(); const auto status = p.status();
    const auto pushed = p.pitchDetector().pushedSamples(), dropped = p.pitchDetector().droppedSamples();
    CHECK(p.captureSnapshot(destination) == expected && same(destination, sentinel), "capture failure preserves destination: %s", label);
    CHECK(p.canRestoreSnapshot(good) == expected && p.restoreSnapshot(good) == expected, "receiver exclusion is explicit: %s", label);
    CHECK(same(d0, p.drift(0).save()) && same(d1, p.drift(1).save()) && same(status, p.status()) &&
              pushed == p.pitchDetector().pushedSamples() && dropped == p.pitchDetector().droppedSamples(),
          "unsupported operation leaves observable live state unchanged: %s", label);
}
void unsupportedDomains() {
    ControlProcessor source; source.prepare(kFs, kBlock, kSeed); warm(source);
    const auto good = capture(source, "supported source");
    ControlProcessor unprepared;
    expectUnsupported(unprepared, good, Result::NotPrepared, "unprepared");
    for (bool enabled : {false, true}) {
        ControlProcessor reference, actual; reference.prepare(kFs, kBlock, kSeed); actual.prepare(kFs, kBlock, kSeed);
        warm(reference); warm(actual); auto values = parameters();
        values[core::indexOf("key.source")] = int(KeySource::Audio); values[core::indexOf("key.enabled")] = enabled ? 1 : 0;
        ControlContext ctx; ctx.frames = enabled ? kBlock : 0;
        processPair(reference, actual, values, ctx, 17, "select audio");
        reference.runPitchWorker(); actual.runPitchWorker();
        expectUnsupported(actual, good, Result::UnsupportedAudioPitch, enabled ? "active audio" : "disabled audio selection");
        reference.runPitchWorker(); actual.runPitchWorker();
        CHECK(reference.pitchDetector().latest().generation == actual.pitchDetector().latest().generation,
              "rejected operations do not add a pitch-worker epoch");
        future(reference, actual, "resume supported mode after audio rejection");
    }
    for (int phase = 0; phase < 3; ++phase) {
        HarmonicMap map;  // even an empty/unavailable bound map is unsupported
        ControlProcessor reference, actual; reference.prepare(kFs, kBlock, kSeed); actual.prepare(kFs, kBlock, kSeed);
        warm(reference); warm(actual); reference.setHarmonicMap(1, &map); actual.setHarmonicMap(1, &map);
        if (phase > 0) {
            ControlContext ctx; ctx.frames = 37;
            processPair(reference, actual, parameters(), ctx, 29, "consume map");
            reference.setHarmonicMap(1, nullptr); actual.setHarmonicMap(1, nullptr);
        }
        if (phase == 2) { reference.resetRealtime(); actual.resetRealtime(); }
        expectUnsupported(actual, good, Result::UnsupportedHarmonicMap,
                          phase == 0 ? "pending map" : phase == 1 ? "in-use map awaiting detach" : "retained map after reset");
        reference.setHarmonicMap(1, nullptr); actual.setHarmonicMap(1, nullptr);
        future(reference, actual, "resume after map detachment");
    }
    ControlProcessor altered; altered.prepare(kFs, kBlock, kSeed); warm(altered);
    altered.drift(0).prepare(44100, kSeed, 0);
    expectUnsupported(altered, good, Result::InvalidState, "mutable drift re-prepared at an incompatible rate");
}

void allocationChecks() {
    checkpoint_allocations::count.store(0); checkpoint_allocations::observing.store(true);
    void* a = ::operator new(7); void* b = ::operator new[](13);
    void* c = ::operator new(11, std::nothrow); void* d = ::operator new[](17, std::nothrow);
    ::operator delete(a); ::operator delete[](b); ::operator delete(c, std::nothrow); ::operator delete[](d, std::nothrow);
    checkpoint_allocations::observing.store(false);
    CHECK(checkpoint_allocations::count.load() == 4, "allocation hooks are calibrated");
    ControlProcessor p; p.prepare(kFs, kBlock, kSeed); warm(p);
    const auto good = capture(p, "allocation fixture"); auto bad = good; bad.slots[1].quantizer.glideRemaining = -1;
    ControlSnapshot destination; bool valid = true;
    checkpoint_allocations::count.store(0); checkpoint_allocations::observing.store(true);
    for (int i = 0; i < 64; ++i) {
        valid = p.captureSnapshot(destination) == Result::Ok && p.canRestoreSnapshot(good) == Result::Ok &&
            p.restoreSnapshot(good) == Result::Ok && p.canRestoreSnapshot(bad) == Result::InvalidState &&
            p.restoreSnapshot(bad) == Result::InvalidState && valid;
    }
    checkpoint_allocations::observing.store(false);
    CHECK(valid, "allocation observation exercised accepted and rejected aggregate paths");
    CHECK(checkpoint_allocations::count.load() == 0, "aggregate snapshot operations use no ordinary/nothrow new/new[]");
}
}  // namespace

int main() {
    continuation(); rejectedCandidates(); unsupportedDomains(); allocationChecks();
    if (g_fail) { std::printf("control checkpoint: %d failure(s)\n", g_fail); return 1; }
    std::printf("control checkpoint: all passed\n");
    return 0;
}
