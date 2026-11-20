// Synthetic control snapshot regressions. Exact future inputs/outputs are the
// continuation oracle; these fixtures do not establish physical validation.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include "TestHarness.h"
#include "control/CreativeDrift.h"
#include "control/KeyTracker.h"
#include "control/ScaleQuantizer.h"
#include "control/SidechainLoad.h"
#include "control/TempoBreathing.h"
#include "control/TransientDetector.h"

// Counts ordinary/nothrow C++ new/new[] in this executable. Deliberately does
// not claim to intercept malloc, aligned allocation, locks, or I/O.
namespace snapshot_allocations {
std::atomic<bool> observing{false};
std::atomic<std::size_t> count{0};
void* allocate(std::size_t bytes) {
    if (observing.load(std::memory_order_relaxed)) count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t n) { return snapshot_allocations::allocate(n); }
void* operator new[](std::size_t n) { return snapshot_allocations::allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return snapshot_allocations::allocate(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return snapshot_allocations::allocate(n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace control_snapshot_tests {
using namespace cd::control;
constexpr double kFs = 48000.0;
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

template<class T> constexpr bool snapshotContract() {
    using State = typename T::State;
    return std::is_trivially_copyable_v<State> && std::is_standard_layout_v<State> &&
        noexcept(std::declval<const T&>().save()) &&
        noexcept(std::declval<const T&>().canRestore(std::declval<const State&>())) &&
        noexcept(std::declval<T&>().restore(std::declval<const State&>()));
}
static_assert(snapshotContract<CreativeDrift>());
static_assert(snapshotContract<TempoBreathing>());
static_assert(snapshotContract<KeyTracker>());
static_assert(snapshotContract<ResonanceQuantizer>());
static_assert(snapshotContract<TransientDetector>());
static_assert(snapshotContract<SidechainLoad>());

// Compare fields rather than padding. Double comparisons are exact because the
// replay has identical future operations, including callback partitioning.
bool same(const CreativeDrift::State& a, const CreativeDrift::State& b) {
    if (a.samples != b.samples || a.ticks != b.ticks || a.q != b.q || a.delayUnitMs != b.delayUnitMs) return false;
    for (int j = 0; j < 3; ++j) {
        if (a.u[j] != b.u[j] || a.rng[j].spare != b.rng[j].spare || a.rng[j].hasSpare != b.rng[j].hasSpare) return false;
        for (int k = 0; k < 4; ++k) if (a.rng[j].s[k] != b.rng[j].s[k]) return false;
    }
    return true;
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
        a.notePitch != b.notePitch || a.weight != b.weight || a.holdLeft != b.holdLeft || a.returnLeft != b.returnLeft)
        return false;
    for (std::size_t i = 0; i < a.notes.size(); ++i)
        if (a.notes[i].note != b.notes[i].note || a.notes[i].held != b.notes[i].held ||
            a.notes[i].sustained != b.notes[i].sustained) return false;
    return true;
}
bool same(const ResonanceQuantizer::State& a, const ResonanceQuantizer::State& b) {
    return a.mask == b.mask && a.root == b.root && a.haveNote == b.haveNote && a.haveOutput == b.haveOutput &&
        a.note == b.note && a.glideNote == b.glideNote && a.current == b.current && a.start == b.start &&
        a.glideRemaining == b.glideRemaining;
}
bool same(const TransientDetector::State& a, const TransientDetector::State& b) {
    return a.sampleRate == b.sampleRate && a.fast == b.fast && a.slow == b.slow && a.held == b.held &&
        a.holdCount == b.holdCount;
}
bool same(const SidechainLoad::State& a, const SidechainLoad::State& b) {
    if (a.sampleRate != b.sampleRate || a.hpfHz != b.hpfHz || a.meanSquare != b.meanSquare ||
        a.envelope != b.envelope || a.budgetAmps != b.budgetAmps || a.lastRatio != b.lastRatio) return false;
    for (int i = 0; i < 2; ++i) if (a.z1[i] != b.z1[i] || a.z2[i] != b.z2[i]) return false;
    return true;
}

template<class T> bool restore(T& target, const typename T::State& state) {
    if constexpr (std::is_void_v<decltype(target.restore(state))>) {
        target.restore(state);
        return true;  // CreativeDrift reports admission separately.
    } else return target.restore(state);
}
template<class T, class Future>
void transfer(T source, T peer, const char* label, Future future) {
    const auto captured = source.save(); const auto peerBefore = peer.save();
    CHECK(source.canRestore(captured), "source admits captured state: %s", label);
    CHECK(peer.canRestore(captured), "compatible peer admits state: %s", label);
    CHECK(same(peer.save(), peerBefore), "admission is pure: %s", label);
    CHECK(restore(peer, captured), "compatible restore succeeds: %s", label);
    CHECK(same(source.save(), captured) && same(peer.save(), captured), "capture/restore preserves fields: %s", label);
    future(source, peer, label);
}
template<class T, class Future>
void reject(T actual, const typename T::State& candidate, const char* label, Future future) {
    // Untouched live reference, not a second restore. Each mutation gets its own
    // full future, so a later successful restore cannot conceal partial changes.
    T reference = actual; const auto before = actual.save();
    CHECK(!actual.canRestore(candidate), "invalid/incompatible state rejected: %s", label);
    CHECK(same(actual.save(), before), "failed admission is pure: %s", label);
    if constexpr (std::is_void_v<decltype(actual.restore(candidate))>) actual.restore(candidate);
    else CHECK(!actual.restore(candidate), "restore reports rejection: %s", label);
    CHECK(same(actual.save(), before), "rejection changes no live fields: %s", label);
    future(actual, reference, label);
}

void driftFuture(CreativeDrift& a, CreativeDrift& b, const char* label) {
    for (int n : {0, -1, 1, 79, 80, 441, 1, 479, 480, 997, 2304, 23}) {
        a.advance(n); b.advance(n);
        CHECK(a.q() == b.q() && a.ticks() == b.ticks() && a.cutoffOct(0.73) == b.cutoffOct(0.73) &&
                  a.delayMs(0.91) == b.delayMs(0.91) && same(a.save(), b.save()),
              "exact future drift, frames %d: %s", n, label);
    }
}
void driftSnapshots() {
    CreativeDrift unprepared;
    CHECK(!unprepared.canRestore(unprepared.save()), "unseeded zero RNG is not a valid drift checkpoint");
    for (double rate : {8000.0, 44100.5, kFs, 192000.0, 768000.0}) {
        CreativeDrift a, peer; a.prepare(rate, 0x123456789ULL, 1); peer.prepare(rate, 0xDEADBEEFULL, 0);
        transfer(a, peer, "fresh drift, different seed and slot", driftFuture);
        a.advance(12345); peer.advance(53);
        transfer(a, peer, "warm drift with pending fractional tick", driftFuture);
    }
    CreativeDrift a, donor; a.prepare(kFs, 357, 0); donor.prepare(kFs, 891, 1);
    a.advance(12345); donor.advance(7003);
    const auto good = a.save(); auto bad = good;
    for (double invalid : {kNan, kInf, -kInf, 1.01, -1.01}) {
        bad = good; bad.q = invalid; reject(a, bad, "drift q nonfinite/bound", driftFuture);
    }
    bad = good; bad.q = good.q > 0 ? -0.5 : 0.5; reject(a, bad, "drift q disagrees with processes", driftFuture);
    for (double invalid : {kNan, kInf, 1.51, -1.51}) {
        bad = good; bad.delayUnitMs = invalid; reject(a, bad, "drift delay nonfinite/bound", driftFuture);
    }
    for (int j = 0; j < 3; ++j) {
        bad = good; bad.u[j] = kNan; reject(a, bad, "drift nonfinite process", driftFuture);
        bad = good; bad.u[j] = 1025; reject(a, bad, "drift process bound", driftFuture);
        bad = good; bad.rng[j].spare = kInf; reject(a, bad, "drift nonfinite spare", driftFuture);
        bad = good; bad.rng[j].spare = -9.01; reject(a, bad, "drift spare bound", driftFuture);
        bad = good; bad.rng[j].hasSpare = 2; reject(a, bad, "drift spare flag", driftFuture);
        bad = good; for (auto& word : bad.rng[j].s) word = 0; reject(a, bad, "drift zero RNG", driftFuture);
    }
    bad = good; ++bad.ticks; reject(a, bad, "drift tick count", driftFuture);
    bad = good; bad.samples = std::numeric_limits<uint64_t>::max(); reject(a, bad, "drift clock mismatch", driftFuture);
    bad = donor.save(); bad.rng[2].hasSpare = 2;
    reject(a, bad, "drift late invalid RNG after different early fields", driftFuture);
    // Tick counts alone cannot identify Fs: 0 and 17 samples are before the first
    // tick at both rates, but the next tick arrives on different samples.
    for (int prefix : {0, 17, 12345}) {
        CreativeDrift source, wrongRate; source.prepare(kFs, 99, 0); wrongRate.prepare(44100.0, 77, 1);
        source.advance(prefix); wrongRate.advance(103);
        reject(wrongRate, source.save(), "drift sample-rate identity", driftFuture);
    }
}

TempoInput tempoInput(bool enabled, double bpm, std::optional<double> ppq, int division = 3, int modifier = 0) {
    TempoInput in; in.enabled = enabled; in.bpm = bpm; in.ppq = ppq;
    in.division = division; in.modifier = modifier; in.depth = 0.81;
    return in;
}
void tempoFuture(TempoBreathing& a, TempoBreathing& b, const char* label) {
    const std::array<TempoInput, 10> inputs = {
        tempoInput(true, 137.0, -7.25), tempoInput(true, 281.0, 17.125, 4, 2),
        tempoInput(true, 54.0, std::nullopt, 2, 1), tempoInput(false, 72.0, -32.0, 5),
        tempoInput(true, 93.0, std::nullopt, 1), tempoInput(true, kNan, std::nullopt, 0, 2),
        tempoInput(true, 191.0, 3.75, 5, 1), tempoInput(true, 120.0, -1e-300, 3),
        tempoInput(true, 233.0, 91.0, 3), tempoInput(false, 120.0, std::nullopt)
    };
    constexpr int frames[] = {0, 31, 117, 0, 0, 257, 479, 1, 960, 9};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto in = inputs[i];
        if (i == 6) { in.timeSigNum = 7; in.timeSigDen = 8; }
        if (i == 8) in.playing = false;
        const auto expected = a.process(in, frames[i]); const auto actual = b.process(in, frames[i]);
        CHECK(same(actual, expected) && same(a.last(), b.last()) && same(a.save(), b.save()),
              "exact future tempo callback %zu: %s", i, label);
        CHECK(a.canRestore(a.save()) && b.canRestore(b.save()), "future tempo state admissible: %s", label);
    }
}
TempoBreathing warmTempo(double fs = kFs) {
    TempoBreathing t; t.prepare(fs);
    t.process(tempoInput(true, 90.0, 3.75), 113);
    t.process(tempoInput(true, 247.0, -17.125, 4, 2), 31);
    return t;
}
void tempoSnapshots() {
    TempoBreathing fresh, peer; peer.prepare(kFs);
    transfer(fresh, peer, "default tempo state", tempoFuture);
    for (double rate : {44100.5, kFs, 96000.0}) {
        auto t = warmTempo(rate); peer.prepare(rate);
        CHECK(t.save().phaseError != 0 && t.save().recoveryTau != t.last().beatsPerCycle * 60.0 /
                  (247.0 * std::log(20.0)), "tempo fixture has live seek and tau smoothing");
        transfer(t, peer, "tempo seek correction and tau smoothing", tempoFuture);
        const auto clock = t.save(); t.process(tempoInput(true, 51.0, 999.0, 0, 1), 0);
        const auto zero = t.save();
        CHECK(zero.phase == clock.phase && zero.phaseError == clock.phaseError && zero.recoveryTau == clock.recoveryTau &&
                  zero.lastPpqEnd == clock.lastPpqEnd && zero.lastBeatsPerCycle == clock.lastBeatsPerCycle &&
                  zero.output.beatsPerCycle != zero.lastBeatsPerCycle,
              "enabled zero-frame tempo caches new parameters without clock advance");
        transfer(t, peer, "tempo enabled zero-frame cache", tempoFuture);
        auto input = tempoInput(false, 83.0, 345.0, 5, 1); input.timeSigNum = 7; input.timeSigDen = 8;
        t.process(input, 2048);
        CHECK(!t.save().wasEnabled && t.last().recoveryTauS == 0 && t.last().cutoffOct == 0 &&
                  t.save().phase == zero.phase && t.save().lastPpqEnd == zero.lastPpqEnd,
              "disabled tempo retains history and reports zero contribution");
        transfer(t, peer, "disabled tempo historical PPQ and cache", tempoFuture);
        t.process(tempoInput(true, 129.0, std::nullopt, 2), 0);
        transfer(t, peer, "tempo re-enabled without clock advance", tempoFuture);
        t.process(tempoInput(true, 178.0, std::nullopt, 1, 2), 151);
        CHECK(!t.save().havePpq, "free-running tempo fixture has no current host PPQ");
        transfer(t, peer, "free-running tempo after host PPQ loss", tempoFuture);
        t.reset(); transfer(t, peer, "reset tempo", tempoFuture);
    }
    auto t = warmTempo(); const auto good = t.save(); auto bad = good;
    for (double v : {kNan, kInf, -1.0, 1.01}) { bad = good; bad.phase = v; reject(t, bad, "tempo phase", tempoFuture); }
    for (double v : {kNan, kInf, -0.51, 0.51}) { bad = good; bad.phaseError = v; reject(t, bad, "tempo correction", tempoFuture); }
    for (double v : {kNan, kInf, -1.0}) {
        bad = good; bad.recoveryTau = v; reject(t, bad, "tempo recovery tau", tempoFuture);
        bad = good; bad.output.recoveryTauS = v; reject(t, bad, "tempo cached tau", tempoFuture);
    }
    bad = good; bad.lastPpqEnd = kInf; reject(t, bad, "tempo PPQ history", tempoFuture);
    bad = good; bad.lastBeatsPerCycle = 0; reject(t, bad, "enabled tempo needs cycle history", tempoFuture);
    bad = good; bad.lastBeatsPerCycle = -1; reject(t, bad, "tempo cycle history bound", tempoFuture);
    for (double v : {kNan, kInf, -0.151, 0.151}) { bad = good; bad.output.cutoffOct = v; reject(t, bad, "tempo cached cutoff", tempoFuture); }
    for (double v : {kNan, 19.0, 301.0}) { bad = good; bad.output.bpmUsed = v; reject(t, bad, "tempo cached BPM", tempoFuture); }
    bad = good; bad.output.phase = 1.01; reject(t, bad, "tempo cached phase", tempoFuture);
    bad = good; bad.output.beatsPerCycle = 0; reject(t, bad, "tempo cached cycle", tempoFuture);
    bad = good; bad.sampleRate = 44100; reject(t, bad, "tempo rate mismatch", tempoFuture);
    bad = good; bad.sampleRate = kNan; reject(t, bad, "tempo nonfinite rate", tempoFuture);
    auto changed = t; changed.process(tempoInput(true, 47.0, 37.125, 0), 761);
    bad = changed.save(); bad.output.beatsPerCycle = kInf;
    reject(t, bad, "tempo late cache rejection after different valid history", tempoFuture);
}

MidiEvent midi(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2 = 0) { return {frame, status, data1, data2}; }
void keyFuture(KeyTracker& a, KeyTracker& b, const char* label) {
    const auto check = [&] {
        CHECK(a.state() == b.state() && a.notePitch() == b.notePitch() && a.weight() == b.weight() &&
                  a.pedalDown() == b.pedalDown() && a.heldCount() == b.heldCount() && same(a.save(), b.save()),
              "exact future key state: %s", label);
        CHECK(a.canRestore(a.save()) && b.canRestore(b.save()), "future key state admissible: %s", label);
    };
    // Let captured hold/return timers run before note-on can replace them.
    for (int frames : {0, 1, 191, 509, 22999, 1, 4799, 1}) {
        a.advanceMidi({}, frames); b.advanceMidi({}, frames); check();
    }
    const MidiEvent ordered[] = {midi(0, 0x90, 60, 90), midi(3, 0x91, 64, 70), midi(6, 0x81, 64),
                                 midi(12, 0x80, 200), midi(13, 0xB0, 64)};
    a.advanceMidi(ordered, 17); b.advanceMidi(ordered, 17); check();
    const MidiEvent release[] = {midi(2, 0x90, 60, 0), midi(2, 0xB0, 123)};
    a.advanceMidi(release, 79); b.advanceMidi(release, 79); check();
    for (int frames : {0, 1, 2399, 2400, 4801}) { a.advanceMidi({}, frames); b.advanceMidi({}, frames); check(); }
    a.advanceAudio(true, 92.25, 31); b.advanceAudio(true, 92.25, 31); check();
    a.advanceAudio(false, 0, 137); b.advanceAudio(false, 0, 137); check();
    a.advanceManual(333); b.advanceManual(333); check();
    const MidiEvent raw[] = {midi(0, 0x90, 255, 127), midi(0, 0xB0, 64, 127), midi(3, 0x80, 255)};
    a.advanceMidi(raw, 19); b.advanceMidi(raw, 19); check();
    const MidiEvent stop[] = {midi(0, 0xB0, 120)};
    a.advanceMidi(stop, 4800); b.advanceMidi(stop, 4800); check();
}
KeyTracker warmKey() {
    KeyTracker k; k.prepare(kFs);
    const MidiEvent events[] = {midi(0, 0x90, 60, 100), midi(3, 0x90, 67, 100), midi(9, 0xB0, 64, 127),
        midi(12, 0x80, 67), midi(15, 0x90, 200, 100), midi(18, 0x90, 60, 100), midi(21, 0x80, 60),
        midi(23, 0x90, 255, 100), midi(25, 0x80, 255)};
    k.advanceMidi(events, 31); return k;
}
void keySnapshots() {
    KeyTracker fresh, peer; peer.prepare(kFs); transfer(fresh, peer, "default key state", keyFuture);
    auto k = warmKey(); const auto ordered = k.save();
    CHECK(ordered.count == 4 && ordered.notePitch == 255 && ordered.pedal && ordered.notes[0].note == 67 &&
              ordered.notes[1].note == 200 && ordered.notes[2].note == 60 && ordered.notes[3].note == 255,
          "ordered recency, sustain and raw uint8 notes are represented");
    transfer(k, peer, "ordered sustained MIDI including notes above 127", keyFuture);
    const MidiEvent release[] = {midi(0, 0x80, 200), midi(2, 0xB0, 64)}; k.advanceMidi(release, 10);
    CHECK(k.state() == KeyState::NoteHold && k.weight() == 1 && k.heldCount() == 0 && k.save().holdLeft == 23992,
          "pedal lift starts the sample-timed 500 ms hold");
    transfer(k, peer, "key hold timer", keyFuture);
    auto inactive = k; inactive.advanceManual(1200);
    CHECK(inactive.state() == KeyState::Returning && inactive.save().holdLeft == k.save().holdLeft && inactive.weight() == 0.75,
          "forced return retains an inactive nonzero hold timer");
    transfer(inactive, peer, "forced return with inactive hold timer", keyFuture);
    inactive.advanceManual(4800);
    CHECK(inactive.state() == KeyState::ManualReference && inactive.save().holdLeft > 0,
          "completed forced return can retain inactive hold timer");
    transfer(inactive, peer, "manual state with retained hold timer", keyFuture);
    for (uint8_t command : {uint8_t(120), uint8_t(123)}) {
        auto immediate = k; const MidiEvent stop[] = {midi(0, 0xB0, command)}; immediate.advanceMidi(stop, 0);
        CHECK(immediate.state() == KeyState::Returning && immediate.save().holdLeft > 0 && immediate.weight() == 1,
              "all-notes-off bypasses hold on a zero-frame callback");
        transfer(immediate, peer, "MIDI immediate release with inactive timer", keyFuture);
    }
    auto audioReturn = k; audioReturn.advanceAudio(false, 0, 17);
    transfer(audioReturn, peer, "audio invalidation bypasses MIDI hold", keyFuture);
    k.advanceMidi({}, 23800); transfer(k, peer, "key near hold expiry", keyFuture);
    k.advanceMidi({}, 500);
    CHECK(k.state() == KeyState::Returning && k.weight() > 0 && k.weight() < 1, "hold reaches natural return ramp");
    transfer(k, peer, "key mid-return", keyFuture);
    k.advanceMidi({}, 10000); transfer(k, peer, "key return complete", keyFuture);
    k.advanceAudio(true, -12345.5, 0); transfer(k, peer, "finite wide-range audio reference", keyFuture);
    k.reset(); transfer(k, peer, "reset key", keyFuture);
    std::array<MidiEvent, 40> notes{};
    for (std::size_t i = 0; i < notes.size(); ++i) notes[i] = midi(uint32_t(i), 0x90, uint8_t(180 + i), 100);
    k.advanceMidi(notes, 40);
    CHECK(k.heldCount() == KeyTracker::kMaxNotes && k.save().notes.front().note == 188 && k.notePitch() == 219,
          "full table evicts oldest note and preserves raw note order");
    transfer(k, peer, "full MIDI table", keyFuture);
    k = warmKey(); const auto good = k.save(); auto bad = good;
    for (int count : {-1, KeyTracker::kMaxNotes + 1}) { bad = good; bad.count = count; reject(k, bad, "key count", keyFuture); }
    for (auto state : {KeyState::Off, static_cast<KeyState>(255)}) {
        bad = good; bad.state = state; reject(k, bad, "key state enum", keyFuture);
    }
    for (double v : {kNan, kInf, -kInf, 32769.0, -32769.0}) { bad = good; bad.notePitch = v; reject(k, bad, "key pitch", keyFuture); }
    for (double v : {kNan, -0.01, 1.01}) { bad = good; bad.weight = v; reject(k, bad, "key weight", keyFuture); }
    for (double v : {kNan, -1.0, 24001.0}) { bad = good; bad.holdLeft = v; reject(k, bad, "key hold counter", keyFuture); }
    for (double v : {kInf, -1.0, 4801.0}) { bad = good; bad.returnLeft = v; reject(k, bad, "key return counter", keyFuture); }
    bad = good; bad.notes[3].note = bad.notes[0].note; reject(k, bad, "key duplicate last note", keyFuture);
    bad = good; bad.notes[3].held = bad.notes[3].sustained = false; reject(k, bad, "key inactive counted note", keyFuture);
    bad = good; bad.pedal = false; reject(k, bad, "key sustain without pedal", keyFuture);
    bad = good; bad.state = KeyState::ManualReference; reject(k, bad, "manual key weight consistency", keyFuture);
    bad = good; bad.weight = 0.5; reject(k, bad, "active key weight consistency", keyFuture);
    bad = good; bad.state = KeyState::Returning; bad.returnLeft = 1; reject(k, bad, "return key weight consistency", keyFuture);
    bad = good; bad.sampleRate = 96000; reject(k, bad, "key rate mismatch", keyFuture);
    bad = good; bad.sampleRate = kNan; reject(k, bad, "key nonfinite rate", keyFuture);
    auto changed = k; changed.advanceAudio(true, 85.75, 1); bad = changed.save(); bad.returnLeft = kInf;
    reject(k, bad, "key late timer rejection after different valid fields", keyFuture);
}

void quantizerFuture(ResonanceQuantizer& a, ResonanceQuantizer& b, const char* label) {
    // Continue the captured decision first, including decide() without update().
    const double capturedTarget = double(a.note());
    CHECK(a.update(capturedTarget, 0.007) == b.update(capturedTarget, 0.007) && same(a.save(), b.save()),
          "quantizer consumes its captured pending decision/glide: %s", label);
    constexpr double pitches[] = {70, 70, 70, 73.7, 73.6, 69.4, 69.66, -12.5, 108.9};
    constexpr double seconds[] = {0, 0.025, 0.025, 0.001, 0.049, 0.002, 0, 0.013, 0.05};
    for (std::size_t i = 0; i < std::size(pitches); ++i) {
        if (i == 4) { a.configure(0x295, -13); b.configure(0x295, -13); }
        if (i == 6) CHECK(a.decide(61.6) == b.decide(61.6), "decision-only continuation: %s", label);
        CHECK(a.update(pitches[i], seconds[i]) == b.update(pitches[i], seconds[i]) &&
                  a.note() == b.note() && a.glided() == b.glided() && same(a.save(), b.save()),
              "exact future quantizer step %zu: %s", i, label);
        CHECK(a.canRestore(a.save()) && b.canRestore(b.save()), "future quantizer state admissible: %s", label);
    }
}
void quantizerSnapshots() {
    ResonanceQuantizer q, peer; peer.configure(0x555, 7); peer.update(43, 0);
    transfer(q, peer, "default quantizer", quantizerFuture);
    ResonanceQuantizer decisionOnly; decisionOnly.configure(scale::maskFor(1, 0), 0); decisionOnly.decide(70);
    CHECK(decisionOnly.save().haveNote && !decisionOnly.save().haveOutput && decisionOnly.note() == 69,
          "decision-only fixture has no initialized output");
    transfer(decisionOnly, peer, "first decision before any output update", quantizerFuture);
    q.update(60, 0); CHECK(q.update(70, 0.025) == 65, "glide fixture exactly halfway from 60 to 70");
    transfer(q, peer, "quantizer mid-glide", quantizerFuture);
    q.configure(scale::maskFor(1, 0), 0);
    CHECK(!q.save().haveNote && q.save().haveOutput && q.save().glideNote == 70 && q.glided() == 65,
          "reconfiguration invalidates decision but retains interrupted glide");
    transfer(q, peer, "reconfigured quantizer before next update", quantizerFuture);
    auto replay = peer; CHECK(replay.restore(q.save()), "reconfigured state transfers");
    CHECK(replay.update(70, 0) == 65 && replay.note() == 69, "new scale changes target without advancing glide");
    CHECK(replay.update(70, 0.025) == 67 && replay.update(70, 0.025) == 69,
          "restored replacement glide starts at captured pitch and completes in 50 ms");
    q.decide(76.0);
    CHECK(q.save().note != q.save().glideNote, "decision-only call may lead the output target");
    transfer(q, peer, "decision-only quantizer before update", quantizerFuture);
    const auto retained = q.save(); q.reset();
    CHECK(!q.save().haveNote && !q.save().haveOutput && q.save().glideRemaining == 0 &&
              q.glided() == retained.current && q.note() == retained.note && q.save().start == retained.start,
          "quantizer reset retains inactive pitch fields");
    transfer(q, peer, "reset quantizer with retained inactive fields", quantizerFuture);
    CHECK(peer.restore(q.save()) && peer.update(72.0, 0) == 72.0, "restored reset initializes first output immediately");
    for (double pitch : {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()}) {
        q.reset(); q.configure(1, 11); q.update(pitch, 0); q.update(-pitch, 0.025);
        transfer(q, peer, "finite pitch saturation with wide glide", quantizerFuture);
    }
    q.reset(); q.configure(scale::kChromatic, 0); q.update(60, 0); q.update(70, 0.025);
    const auto good = q.save(); auto bad = good;
    for (uint16_t mask : {uint16_t(0), uint16_t(0x1000), uint16_t(0xFFFF)}) {
        bad = good; bad.mask = mask; reject(q, bad, "quantizer mask", quantizerFuture);
    }
    for (int root : {-1, 12}) { bad = good; bad.root = root; reject(q, bad, "quantizer root", quantizerFuture); }
    for (int note : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        bad = good; bad.note = note; reject(q, bad, "quantizer note domain", quantizerFuture);
        bad = good; bad.glideNote = note; reject(q, bad, "quantizer glide note domain", quantizerFuture);
    }
    for (double v : {kNan, kInf, double(std::numeric_limits<int>::max())}) {
        bad = good; bad.current = v; reject(q, bad, "quantizer current pitch", quantizerFuture);
        bad = good; bad.start = v; reject(q, bad, "quantizer glide start", quantizerFuture);
    }
    for (double v : {kNan, kInf, -0.001, 0.051}) { bad = good; bad.glideRemaining = v; reject(q, bad, "quantizer duration", quantizerFuture); }
    bad = good; bad.haveOutput = false; reject(q, bad, "uninitialized quantizer cannot have active glide", quantizerFuture);
    bad = good; bad.mask = 1; reject(q, bad, "retained quantizer decision must be allowed", quantizerFuture);
    bad = good; bad.current += 0.125; reject(q, bad, "quantizer interpolation consistency", quantizerFuture);
    auto changed = q; changed.update(81, 0.01); bad = changed.save(); bad.glideRemaining = -1;
    reject(q, bad, "quantizer late rejection after different current/start/target", quantizerFuture);
}

void transientFuture(TransientDetector& a, TransientDetector& b, const char* label) {
    std::array<double, 512> left{}, right{}, expected{}, actual{};
    for (int block = 0; block < 12; ++block) {
        const int frames = block == 0 ? 0 : block == 1 ? 1 : block == 2 ? 31 : 512;
        for (int i = 0; i < frames; ++i) {
            left[std::size_t(i)] = block < 6 ? 0.0 : (i % 23 == 0 ? 0.8 : 0.002);
            right[std::size_t(i)] = block < 8 ? 0.0 : (i % 37 == 0 ? -0.91 : 0.003);
        }
        expected.fill(-7); actual.fill(-7);
        const double amount = block == 3 ? 0.0 : block == 4 ? kNan : 0.73;
        const double* r = block % 2 ? right.data() : nullptr;
        a.process(left.data(), r, frames, amount, expected.data());
        b.process(left.data(), r, frames, amount, actual.data());
        CHECK(expected == actual && same(a.save(), b.save()), "exact future transient samples, block %d: %s", block, label);
        CHECK(a.canRestore(a.save()) && b.canRestore(b.save()), "future transient state admissible: %s", label);
        for (int i = 0; i < frames; ++i)
            CHECK(std::isfinite(actual[std::size_t(i)]) && actual[std::size_t(i)] >= 0 && actual[std::size_t(i)] <= 1 &&
                      ((block != 3 && block != 4) || actual[std::size_t(i)] == 0),
                  "transient output domain/zero amount, sample %d: %s", i, label);
    }
}
void transientSnapshots() {
    TransientDetector unprepared, t, peer; t.prepare(kFs); peer.prepare(kFs);
    CHECK(!unprepared.canRestore(unprepared.save()) && !unprepared.canRestore(t.save()) && !unprepared.restore(t.save()),
          "unprepared transient coefficients cannot admit snapshots");
    CHECK(unprepared.save().sampleRate == 0, "unprepared transient rejection preserves identity");
    transfer(t, peer, "fresh prepared transient", transientFuture);
    double impulse = 1.0, silence = 0.0, out = -1;
    t.process(&impulse, nullptr, 1, 1.0, &out);
    CHECK(t.save().fast > t.save().slow && t.save().slow > 0 && out == 1,
          "transient attack fixture has envelope separation and full preservation");
    transfer(t, peer, "transient attack", transientFuture);
    bool foundHold = false, foundRelease = false;
    for (int i = 0; i < 6000 && !foundRelease; ++i) {
        t.process(&silence, nullptr, 1, 1, &out); const auto state = t.save();
        if (!foundHold && state.held == 1 && state.holdCount > 0 && state.holdCount < 240) {
            foundHold = true; transfer(t, peer, "transient partway through peak hold", transientFuture);
        }
        if (state.held > 0 && state.held < 1 && state.holdCount == 0) {
            foundRelease = true; transfer(t, peer, "transient release envelope", transientFuture);
            const double before = out; t.process(&silence, nullptr, 1, 1, &out);
            CHECK(out < before && out > 0, "transient release continues after capture");
        }
    }
    CHECK(foundHold && foundRelease, "public impulse/silence reaches both hold and release captures");
    t.reset(); transfer(t, peer, "reset transient", transientFuture);
    impulse = std::numeric_limits<double>::max(); t.process(&impulse, nullptr, 1, 1, &out);
    CHECK(std::isfinite(t.save().fast) && std::isfinite(t.save().slow), "large finite transient has finite captured envelopes");
    transfer(t, peer, "large finite transient envelopes", transientFuture);
    t.reset(); impulse = 0.7; t.process(&impulse, nullptr, 1, 1, &out);
    const auto good = t.save(); auto bad = good;
    for (double v : {kNan, kInf, -1.0}) {
        bad = good; bad.fast = v; reject(t, bad, "transient fast envelope", transientFuture);
        bad = good; bad.slow = v; reject(t, bad, "transient slow envelope", transientFuture);
    }
    for (double v : {kNan, kInf, -0.01, 1.01}) { bad = good; bad.held = v; reject(t, bad, "transient held strength", transientFuture); }
    for (int count : {-1, 241, std::numeric_limits<int>::max()}) { bad = good; bad.holdCount = count; reject(t, bad, "transient hold count", transientFuture); }
    bad = good; bad.sampleRate = 44100; reject(t, bad, "transient rate mismatch", transientFuture);
    bad = good; bad.sampleRate = kNan; reject(t, bad, "transient nonfinite rate", transientFuture);
    reject(t, unprepared.save(), "transient unprepared source", transientFuture);
    auto changed = t; impulse = 0.2; changed.process(&impulse, nullptr, 1, 0.4, &out); bad = changed.save(); bad.holdCount = -1;
    reject(t, bad, "transient late rejection after different envelopes", transientFuture);
}

void sidechainFuture(SidechainLoad& a, SidechainLoad& b, const char* label) {
    std::array<double, 512> left{}, right{}, expected{}, actual{};
    double hpf = a.save().hpfHz;
    for (int block = 0; block < 11; ++block) {
        const int frames = block == 0 ? 1 : block == 1 ? 31 : block == 3 || block == 5 ? 0 : 512;
        for (int i = 0; i < frames; ++i) {
            left[std::size_t(i)] = 0.19 * std::sin(0.097 * (i + 113 * block));
            right[std::size_t(i)] = 0.13 * std::cos(0.141 * (i + 71 * block));
        }
        if (block == 3) hpf = 277;
        if (block == 5) hpf = 23;
        if (block == 6) { a.setBudgetAmps(0.031); b.setBudgetAmps(0.031); }
        const auto source = block == 3 || block == 8 ? SidechainSource::Main :
                            block % 2 ? SidechainSource::Blend : SidechainSource::External;
        const bool connected = block != 5 && block != 9;
        const double depth = block == 7 ? 0.0 : 1.3;
        expected.fill(-19); actual.fill(-19);
        const double* r = block % 2 ? right.data() : nullptr;
        const bool usedA = a.process(source, depth, hpf, left.data(), r, connected, frames, expected.data());
        const bool usedB = b.process(source, depth, hpf, left.data(), r, connected, frames, actual.data());
        CHECK(usedA == usedB && expected == actual && a.lastEnergyRatio() == b.lastEnergyRatio() &&
                  a.budgetAmps() == b.budgetAmps() && same(a.save(), b.save()),
              "exact future sidechain samples/selection, block %d: %s", block, label);
        CHECK(a.canRestore(a.save()) && b.canRestore(b.save()), "future sidechain state admissible: %s", label);
        for (int i = 0; i < frames; ++i)
            CHECK(std::isfinite(actual[std::size_t(i)]) && actual[std::size_t(i)] >= 0 &&
                      ((block != 7 && block != 8 && block != 9) || actual[std::size_t(i)] == 0),
                  "sidechain finite current/zero selection, sample %d: %s", i, label);
        if (frames < int(actual.size())) CHECK(actual[std::size_t(frames)] == -19, "sidechain writes only its sample span: %s", label);
    }
}
SidechainLoad warmSidechain() {
    SidechainLoad s; s.prepare(kFs); s.setBudgetAmps(0.027);
    std::array<double, 733> left{}, right{}, out{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 0.2 * std::sin(0.121 * double(i)); right[i] = 0.11 * std::cos(0.177 * double(i));
    }
    s.process(SidechainSource::External, 0.83, 88.0, left.data(), right.data(), true, int(left.size()), out.data());
    return s;
}
void sidechainSnapshots() {
    SidechainLoad unprepared, s, peer; s.prepare(kFs); peer.prepare(kFs); peer.setBudgetAmps(0.9);
    CHECK(!unprepared.canRestore(unprepared.save()) && !unprepared.canRestore(s.save()) && !unprepared.restore(s.save()),
          "unprepared sidechain coefficients cannot admit snapshots");
    CHECK(unprepared.save().sampleRate == 0 && unprepared.save().hpfHz == -1, "unprepared sidechain rejection preserves identity");
    transfer(s, peer, "fresh prepared sidechain", sidechainFuture);
    s = warmSidechain(); const auto warm = s.save();
    CHECK(warm.z1[0] != 0 && warm.z1[1] != 0 && warm.z2[0] != 0 && warm.z2[1] != 0 &&
              warm.meanSquare > 0 && warm.envelope > 0 && warm.lastRatio > 0,
          "sidechain fixture has both HPF histories, mean square and envelope history");
    transfer(s, peer, "warm stereo sidechain", sidechainFuture);
    std::array<double, 2> untouched{-37, 19};
    CHECK(!s.process(SidechainSource::Main, 0, 233, nullptr, nullptr, false, 0, untouched.data()),
          "zero-frame HPF edit reports supplied bus selection");
    const auto zero = s.save(); auto expectedZero = warm; expectedZero.hpfHz = 233;
    CHECK(same(zero, expectedZero) && untouched[0] == -37 && untouched[1] == 19,
          "zero-frame HPF edit preserves all signal history and output storage");
    transfer(s, peer, "sidechain immediately after zero-frame HPF edit", sidechainFuture);
    s.reset();
    CHECK(s.budgetAmps() == warm.budgetAmps && s.save().hpfHz == 233 && s.lastEnergyRatio() == 0,
          "sidechain reset retains budget and filter setting");
    transfer(s, peer, "reset sidechain retains filter and budget", sidechainFuture);
    for (double rate : {8000.0, 44100.5, 768000.0}) {
        s.prepare(rate); peer.prepare(rate); s.setBudgetAmps(std::numeric_limits<double>::max());
        const std::array<double, 8> large = {0x1p499, -0x1p499, 0x1p500, -0x1p500,
                                            0x1p501, kNan, kInf, std::numeric_limits<double>::max()};
        std::array<double, 8> out{};
        s.process(SidechainSource::External, 2, 20, large.data(), large.data(), true, int(large.size()), out.data());
        CHECK(s.budgetAmps() == std::numeric_limits<double>::max() / 8 && s.save().meanSquare > 0 &&
                  s.save().envelope > 0 && s.canRestore(s.save()), "large finite sidechain state admissible at %g Hz", rate);
        for (double current : out) CHECK(std::isfinite(current) && current >= 0, "large finite sidechain current representable");
        transfer(s, peer, "large finite history and maximum budget", sidechainFuture);
    }
    s = warmSidechain(); const auto good = s.save(); auto bad = good;
    for (double v : {kNan, kInf, 19.0, 301.0}) { bad = good; bad.hpfHz = v; reject(s, bad, "sidechain HPF setting", sidechainFuture); }
    for (int ch = 0; ch < 2; ++ch) for (double v : {kNan, kInf, 0x1p501, -0x1p501}) {
        bad = good; bad.z1[ch] = v; reject(s, bad, "sidechain first HPF history", sidechainFuture);
        bad = good; bad.z2[ch] = v; reject(s, bad, "sidechain second HPF history", sidechainFuture);
    }
    for (double v : {kNan, kInf, -1.0, 0x1p1001}) { bad = good; bad.meanSquare = v; reject(s, bad, "sidechain mean square", sidechainFuture); }
    for (double v : {kNan, kInf, -1.0, 0x1p501}) { bad = good; bad.envelope = v; reject(s, bad, "sidechain envelope", sidechainFuture); }
    for (double v : {kNan, kInf, -1.0, std::numeric_limits<double>::max()}) {
        bad = good; bad.budgetAmps = v; reject(s, bad, "sidechain budget", sidechainFuture);
    }
    bad = good; bad.budgetAmps = std::nextafter(std::numeric_limits<double>::max() / 8, kInf);
    reject(s, bad, "sidechain budget immediately above representable cap", sidechainFuture);
    for (double v : {kNan, kInf, -1.0}) { bad = good; bad.lastRatio = v; reject(s, bad, "sidechain cached ratio", sidechainFuture); }
    bad = good; bad.lastRatio = std::nextafter(good.lastRatio, kInf);
    reject(s, bad, "sidechain ratio inconsistent with envelope", sidechainFuture);
    bad = good; bad.sampleRate = 44100; reject(s, bad, "sidechain rate mismatch", sidechainFuture);
    bad = good; bad.sampleRate = kNan; reject(s, bad, "sidechain nonfinite rate", sidechainFuture);
    reject(s, unprepared.save(), "sidechain unprepared source", sidechainFuture);
    auto changed = s; changed.setBudgetAmps(0.071);
    changed.process(SidechainSource::Main, 0, 291, nullptr, nullptr, false, 0, nullptr);
    bad = changed.save(); bad.z2[1] = kInf;
    reject(s, bad, "sidechain late channel rejection after different HPF and budget", sidechainFuture);
}

void allocationChecks() {
    // Calibrate both replacement entry points explicitly: an ineffective counter
    // must not make the zero-allocation assertion pass vacuously.
    snapshot_allocations::count.store(0, std::memory_order_relaxed);
    snapshot_allocations::observing.store(true, std::memory_order_relaxed);
    void* scalar = ::operator new(7); void* array = ::operator new[](13);
    void* scalarNothrow = ::operator new(11, std::nothrow); void* arrayNothrow = ::operator new[](17, std::nothrow);
    ::operator delete(scalar); ::operator delete[](array);
    ::operator delete(scalarNothrow, std::nothrow); ::operator delete[](arrayNothrow, std::nothrow);
    snapshot_allocations::observing.store(false, std::memory_order_relaxed);
    CHECK(snapshot_allocations::count.load(std::memory_order_relaxed) == 4, "ordinary/nothrow new/new[] counters are live");
    CreativeDrift drift; drift.prepare(kFs, 19, 0); drift.advance(1703);
    auto tempo = warmTempo(); auto key = warmKey(); auto sidechain = warmSidechain();
    ResonanceQuantizer quantizer; quantizer.update(60, 0); quantizer.update(72, 0.025);
    TransientDetector transient; transient.prepare(kFs); double input = 1, output = 0;
    transient.process(&input, nullptr, 1, 1, &output);
    auto badDrift = drift.save(); badDrift.rng[2].hasSpare = 2;
    auto badTempo = tempo.save(); badTempo.output.beatsPerCycle = kInf;
    auto badKey = key.save(); badKey.returnLeft = kInf;
    auto badQuantizer = quantizer.save(); badQuantizer.glideRemaining = -1;
    auto badTransient = transient.save(); badTransient.holdCount = -1;
    auto badSidechain = sidechain.save(); badSidechain.z2[1] = kInf;
    bool valid = true;
    snapshot_allocations::count.store(0, std::memory_order_relaxed);
    snapshot_allocations::observing.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i) {
        const auto d = drift.save(); const auto t = tempo.save(); const auto k = key.save();
        const auto q = quantizer.save(); const auto tr = transient.save(); const auto s = sidechain.save();
        valid = drift.canRestore(d) && tempo.canRestore(t) && key.canRestore(k) && quantizer.canRestore(q) &&
                transient.canRestore(tr) && sidechain.canRestore(s) && valid;
        drift.restore(d);
        valid = tempo.restore(t) && key.restore(k) && quantizer.restore(q) && transient.restore(tr) && sidechain.restore(s) && valid;
        valid = !drift.canRestore(badDrift) && !tempo.canRestore(badTempo) && !key.canRestore(badKey) &&
                !quantizer.canRestore(badQuantizer) && !transient.canRestore(badTransient) && !sidechain.canRestore(badSidechain) && valid;
        drift.restore(badDrift);
        valid = !tempo.restore(badTempo) && !key.restore(badKey) && !quantizer.restore(badQuantizer) &&
                !transient.restore(badTransient) && !sidechain.restore(badSidechain) && valid;
    }
    snapshot_allocations::observing.store(false, std::memory_order_relaxed);
    CHECK(valid, "measured valid and rejecting snapshot operations took intended paths");
    CHECK(snapshot_allocations::count.load(std::memory_order_relaxed) == 0,
          "save/canRestore/restore use no ordinary or nothrow C++ new/new[]");
}
void run() {
    driftSnapshots(); tempoSnapshots(); keySnapshots(); quantizerSnapshots(); transientSnapshots(); sidechainSnapshots();
    allocationChecks();
}
}  // namespace control_snapshot_tests

// The aggregate suite can be a separate header/run() beside this leaf-suite call
// once its API is available. No provisional ControlProcessor API is assumed here.
int main() {
    control_snapshot_tests::run();
    if (g_fail) { std::printf("control snapshots: %d failure(s)\n", g_fail); return 1; }
    std::printf("control snapshots: all passed\n");
    return 0;
}
