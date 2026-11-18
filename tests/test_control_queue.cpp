// Acceptance tests for src/control (P04 queue/snapshot interface). Synthetic payloads only.
// Covers: SPSC ring, latest-slot coherence, sample clock partition invariance (incl. zero frames),
// queue pressure policy, generation rejection, stopped-audio save fallback, off-audio reclamation,
// and concurrent save/restore/config/events against a running "audio" thread.
// Limits: allocation on the audio thread is trapped here via a test-local operator new/delete
// counter (positive control included); lock and OS-I/O traps are NOT implemented (UX-09 packet 2).
#include "control/ControlHub.h"
#include "control/Handoff.h"
#include "control/LatestSlot.h"
#include "control/SampleClock.h"
#include "control/SpscRing.h"
#include "core/ParameterRegistry.h"
#include "state/StateDocument.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace cd;
using namespace cd::control;
using std::chrono::microseconds;

int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- allocation trap (test-local) ----
thread_local bool tl_audio = false;
std::atomic<std::uint64_t> g_audioAllocs{0}, g_audioFrees{0};
struct AudioScope {
    AudioScope() { tl_audio = true; }
    ~AudioScope() { tl_audio = false; }
};
}  // namespace

void* operator new(std::size_t n) {
    if (tl_audio) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (tl_audio) g_audioFrees.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace {

std::vector<std::size_t> floatParams(std::size_t n) {
    std::vector<std::size_t> v;
    for (std::size_t i = 0; i < core::kParamCount && v.size() < n; ++i)
        if (core::kParams[i].kind == core::ParamKind::Float && core::kParams[i].max > core::kParams[i].min) v.push_back(i);
    return v;
}
double valueAt(std::size_t idx, double frac) {
    const auto& p = core::kParams[idx];
    return p.min + (p.max - p.min) * frac;
}
double fracOf(std::size_t idx, double v) {
    const auto& p = core::kParams[idx];
    return (v - p.min) / (p.max - p.min);
}

// ------------------------------------------------------------------ ring / slot
void testRing() {
    SpscRing<int, 4> r;
    int v = 0;
    CHECK(!r.tryPop(v));
    CHECK(r.freeSlots() == 4);
    for (int i = 0; i < 4; ++i) CHECK(r.tryPush(i));
    CHECK(!r.tryPush(99));  // full: rejected, nothing overwritten
    CHECK(r.size() == 4 && r.freeSlots() == 0);
    for (int i = 0; i < 4; ++i) { CHECK(r.tryPop(v)); CHECK(v == i); }
    for (int round = 0; round < 10; ++round) {  // wrap-around
        CHECK(r.tryPush(round)); CHECK(r.tryPush(round + 100));
        CHECK(r.tryPop(v) && v == round); CHECK(r.tryPop(v) && v == round + 100);
    }
    // threaded FIFO integrity
    SpscRing<std::uint64_t, 64> q;
    constexpr std::uint64_t kN = 200000;
    std::atomic<bool> bad{false};
    std::thread cons([&] {
        std::uint64_t expect = 0, x = 0;
        while (expect < kN) {
            if (q.tryPop(x)) { if (x != expect) bad = true; ++expect; }
            else std::this_thread::yield();
        }
    });
    for (std::uint64_t i = 0; i < kN;) { if (q.tryPush(i)) ++i; else std::this_thread::yield(); }
    cons.join();
    CHECK(!bad.load());
}

struct Wide { std::uint64_t a[16]; };
void testLatestSlot() {
    LatestSlot<Wide> s;
    CHECK(s.acquire() == nullptr);
    for (int k = 1; k <= 2; ++k) { for (auto& w : s.writeBuffer().a) w = k; s.publish(); }
    const Wide* p = s.acquire();
    CHECK(p != nullptr && p->a[0] == 2);  // coalesced: only the latest survives
    CHECK(s.acquire() == nullptr);
    // threaded coherence + monotonic
    LatestSlot<Wide> t;
    std::atomic<bool> stop{false}, bad{false};
    std::thread cons([&] {
        std::uint64_t last = 0;
        while (!stop.load()) {
            if (const Wide* w = t.acquire()) {
                for (auto x : w->a) if (x != w->a[0]) bad = true;
                if (w->a[0] < last) bad = true;
                last = w->a[0];
            }
        }
    });
    for (std::uint64_t i = 1; i <= 200000; ++i) { for (auto& w : t.writeBuffer().a) w = i; t.publish(); }
    stop = true;
    cons.join();
    CHECK(!bad.load());
}

// ------------------------------------------------------------------ clock
void testClockPartitions() {
    const std::uint32_t parts[] = {1, 17, 64, 127, 512, 8192};
    const std::uint32_t rates[] = {44100, 48000, 96000, 192000};
    const std::uint64_t total = 100003;
    for (std::uint32_t rate : rates) {
        const std::uint64_t expTicks = total * 1000 / rate, expRem = total * 1000 % rate;
        for (std::uint32_t p : parts) {
            SampleClock c;
            CHECK(c.prepare(rate, 1000));
            std::uint64_t ticks = 0, done = 0;
            while (done < total) {
                const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(p, total - done));
                ticks += c.advance(0);  // zero-frame call between blocks advances nothing
                ticks += c.advance(n);
                done += n;
            }
            CHECK(c.advance(0) == 0);
            CHECK(ticks == expTicks && c.remainder() == expRem && c.sampleIndex() == total);
        }
    }
    SampleClock c;
    CHECK(!c.prepare(0, 1) && !c.prepare(48000, 0) && !c.prepare(48000, 48001) && !c.prepare(1u << 21, 1));
    CHECK(!c.prepared());
    CHECK(c.advance(10) == 0);  // unprepared: index only, no ticks
    CHECK(c.prepare(48000, 1000));
    CHECK(!c.restore(5, 44100, 0));    // different rate
    CHECK(!c.restore(5, 48000, 48000)); // remainder out of range
    CHECK(c.restore(12345, 48000, 47999) && c.sampleIndex() == 12345 && c.remainder() == 47999);
    CHECK(c.advance(1) == 1 && c.remainder() == 999);  // 47999+1000 = 48999 -> 1 tick, rem 999
}

// ------------------------------------------------------------------ hub: partitioned blocks
void testHubPartitions() {
    const std::uint32_t parts[] = {1, 17, 64, 127, 512, 8192};
    const std::uint64_t total = 96037;
    const auto fp = floatParams(1);
    const std::size_t ia = fp[0];
    std::uint64_t refTicks = 0, refRem = 0;
    for (std::uint32_t p : parts) {
        auto hubp = std::make_unique<ControlHub>();
        ControlHub& hub = *hubp;
        CHECK(hub.prepareClock(48000, 1000));
        std::uint64_t done = 0, ticks = 0, appliedAt = ~0ull, committedAt = ~0ull;
        std::uint32_t blockNo = 0;
        while (done < total) {
            if (committedAt == ~0ull && done >= 1000) {
                CHECK(hub.stage(core::kParams[ia].hostId, valueAt(ia, 0.25)));
                hub.commit();
                committedAt = done;
            }
            const std::uint32_t n = (blockNo % 5 == 2) ? 0u : static_cast<std::uint32_t>(std::min<std::uint64_t>(p, total - done));
            const BlockControl& b = hub.beginBlock(n);
            if (b.configChanged && b.eventCount == 0 && std::fabs(hub.currentConfig().values[ia] - valueAt(ia, 0.25)) < 1e-15)
                if (appliedAt == ~0ull) appliedAt = hub.clock().sampleIndex();
            ticks += hub.endBlock();
            done += n;
            ++blockNo;
        }
        CHECK(committedAt != ~0ull && appliedAt == committedAt);  // applied at the very next block boundary
        CHECK(hub.clock().sampleIndex() == total);
        if (p == parts[0]) { refTicks = ticks; refRem = hub.clock().remainder(); }
        CHECK(ticks == refTicks && hub.clock().remainder() == refRem);
        CHECK(ticks == total * 1000 / 48000);
    }
    // Zero-frame callback applies state but does not advance the clock.
    auto hubp = std::make_unique<ControlHub>();
    ControlHub& hub = *hubp;
    hub.prepareClock(48000, 1000);
    hub.beginBlock(100); hub.endBlock();
    hub.stage(core::kParams[ia].hostId, valueAt(ia, 0.75));
    const std::uint64_t rev = hub.commit();
    const BlockControl& z = hub.beginBlock(0);
    CHECK(z.configChanged && hub.currentConfig().revision == rev && z.frames == 0);
    CHECK(hub.endBlock() == 0 && hub.clock().sampleIndex() == 100);
    CHECK(hub.endBlock() == 0 && hub.clock().sampleIndex() == 100);  // repeated endBlock cannot advance
}

// ------------------------------------------------------------------ hub: queue pressure
void testQueuePressure() {
    auto hubp = std::make_unique<ControlHub>();
    ControlHub& hub = *hubp;
    hub.prepareClock(48000, 1000);
    const auto fp = floatParams(10);
    CHECK(fp.size() == 10);
    hub.beginBlock(0); hub.endBlock();  // adopt generation, initial config

    // Invalid input is rejected and counted, never clamped.
    const std::uint32_t id0 = core::kParams[fp[0]].hostId;
    const double before = hub.committedConfig().values[fp[0]];
    CHECK(!hub.sendParam(99999, 0.5));
    CHECK(!hub.sendParam(id0, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!hub.sendParam(id0, std::numeric_limits<double>::infinity()));
    CHECK(!hub.stage(id0, core::kParams[fp[0]].max * 2 + 1));
    CHECK(!hub.stage(99998, 0));
    CHECK(hub.diagnostics().inputRejected == 5);
    CHECK(hub.committedConfig().values[fp[0]] == before);

    // Fill the ring (256) with no consumer, then keep sending: later values park, latest wins.
    const int kSends = 1000;
    auto sendVal = [&](int i) { return valueAt(fp[i % 10], (i % 97) / 97.0); };
    for (int i = 0; i < kSends; ++i) CHECK(hub.sendParam(core::kParams[fp[i % 10]].hostId, sendVal(i)));
    Diagnostics d = hub.diagnostics();
    CHECK(d.eventsDeferred == 10);
    CHECK(d.eventsCoalesced == static_cast<std::uint64_t>(kSends) - kEventRingCapacity - 10);
    CHECK(hub.flushEvents() == 10);  // ring still full: nothing moved, nothing lost

    const BlockControl& b1 = hub.beginBlock(64);
    CHECK(b1.eventCount == kEventRingCapacity);
    for (std::uint32_t i = 0; i < b1.eventCount; ++i)  // FIFO order and exact values
        CHECK(b1.events[i].hostId == core::kParams[fp[i % 10]].hostId && b1.events[i].value == sendVal(static_cast<int>(i)));
    hub.endBlock();
    CHECK(hub.flushEvents() == 0);  // space now available: parked latest values enter the ring
    const BlockControl& b2 = hub.beginBlock(64);
    CHECK(b2.eventCount == 10);
    for (std::uint32_t i = 0; i < b2.eventCount; ++i) {  // final value of every parameter survived
        const int last = 990 + (static_cast<int>(kEventRingCapacity + i) % 10);  // first-parked order
        CHECK(b2.events[i].hostId == core::kParams[fp[last % 10]].hostId && b2.events[i].value == sendVal(last));
    }
    hub.endBlock();
    d = hub.diagnostics();
    CHECK(d.eventsStale == 0);

    // Commit supersedes parked values: configuration carries the final values.
    for (int i = 0; i < kSends; ++i) hub.sendParam(core::kParams[fp[i % 10]].hostId, sendVal(i));
    CHECK(hub.diagnostics().eventsDeferred == 20);
    hub.commit();
    CHECK(hub.diagnostics().eventsSuperseded == 10);
    const BlockControl& b3 = hub.beginBlock(64);
    CHECK(b3.configChanged);
    for (int j = 0; j < 10; ++j) CHECK(hub.currentConfig().values[fp[j]] == sendVal(990 + j));
    // The 256 queued events are older than the commit... they carry the earlier revision: dropped, not applied.
    CHECK(b3.eventCount == 0 && hub.diagnostics().eventsStale == kEventRingCapacity);
    hub.endBlock();
}

// ------------------------------------------------------------------ hub: generations
CheckpointBlock makeCp(std::uint64_t index, std::uint32_t rate, std::uint64_t rem, double fill) {
    CheckpointBlock cp;
    cp.sampleIndex = index; cp.sampleRateHz = rate; cp.clockRemainder = rem;
    cp.electricalCount = 4; cp.thermalCount = 2; cp.rngCount = 1;
    for (int i = 0; i < 4; ++i) cp.electrical[i] = fill;
    cp.thermal[0] = cp.thermal[1] = fill;
    cp.rng[0] = static_cast<std::uint64_t>(fill);
    return cp;
}

void testGenerations() {
    auto hubp = std::make_unique<ControlHub>();
    ControlHub& hub = *hubp;
    hub.prepareClock(48000, 1000);
    const auto fp = floatParams(1);
    const std::uint32_t id = core::kParams[fp[0]].hostId;
    CHECK(hub.generation() == 1);
    CHECK(hub.beginBlock(0).generationChanged && hub.audioGeneration() == 1 && hub.currentConfig().revision == 1);
    hub.endBlock();

    // Old-generation events and config are rejected, never applied.
    for (int i = 0; i < 3; ++i) CHECK(hub.sendParam(id, valueAt(fp[0], 0.1 * (i + 1))));
    const std::uint64_t restoreOld = hub.requestRestore(makeCp(777, 48000, 5, 1), hub.generation());
    CHECK(restoreOld == 1);
    const std::uint64_t g2 = hub.beginGeneration(0xABCD);
    CHECK(g2 == 2 && hub.generation() == 2);
    const BlockControl& b = hub.beginBlock(32);
    CHECK(b.generationChanged && hub.audioGeneration() == 2);
    CHECK(b.eventCount == 0 && hub.diagnostics().eventsStale == 3);
    CHECK(b.configChanged && hub.currentConfig().generation == 2 && hub.currentConfig().revision >= 2);
    CHECK(b.restore == nullptr && hub.restoreStatus(restoreOld) == RestoreStatus::RejectedStaleGeneration);
    hub.endBlock();
    CHECK(hub.clock().sampleIndex() == 32);  // stale restore did not touch the clock

    // Valid restore applies after the engine commits it; clock follows.
    const std::uint64_t ok = hub.requestRestore(makeCp(5000, 48000, 123, 9), 2);
    CHECK(hub.restoreStatus(ok) == RestoreStatus::Pending);
    const BlockControl& br = hub.beginBlock(16);
    CHECK(br.restore != nullptr && br.restore->rng[0] == 9 && br.restore->generation == 2);
    hub.commitRestore(true);
    CHECK(hub.restoreStatus(ok) == RestoreStatus::Applied);
    CHECK(hub.clock().sampleIndex() == 5000 && hub.clock().remainder() == 123);
    CHECK(hub.endBlock() == (123 + 16 * 1000) / 48000);
    CHECK(hub.clock().sampleIndex() == 5016);

    // Incompatible sample rate / remainder; engine decline; uncommitted; superseded; malformed.
    const std::uint64_t badRate = hub.requestRestore(makeCp(1, 44100, 0, 1), 2);
    CHECK(hub.beginBlock(0).restore == nullptr && hub.restoreStatus(badRate) == RestoreStatus::RejectedIncompatible);
    hub.endBlock();
    const std::uint64_t badRem = hub.requestRestore(makeCp(1, 48000, 48000, 1), 2);
    CHECK(hub.beginBlock(0).restore == nullptr && hub.restoreStatus(badRem) == RestoreStatus::RejectedIncompatible);
    hub.endBlock();
    const std::uint64_t declined = hub.requestRestore(makeCp(1, 48000, 0, 1), 2);
    CHECK(hub.beginBlock(0).restore != nullptr);
    hub.commitRestore(false);
    CHECK(hub.restoreStatus(declined) == RestoreStatus::RejectedIncompatible && hub.clock().sampleIndex() == 5016);
    hub.endBlock();
    const std::uint64_t forgot = hub.requestRestore(makeCp(1, 48000, 0, 1), 2);
    CHECK(hub.beginBlock(0).restore != nullptr);
    hub.endBlock();  // engine never committed
    CHECK(hub.restoreStatus(forgot) == RestoreStatus::NotCommitted && hub.clock().sampleIndex() == 5016);
    const std::uint64_t s1 = hub.requestRestore(makeCp(10, 48000, 0, 1), 2);
    const std::uint64_t s2 = hub.requestRestore(makeCp(20, 48000, 0, 2), 2);
    const BlockControl& bs = hub.beginBlock(0);
    CHECK(bs.restore != nullptr && bs.restore->rng[0] == 2);
    hub.commitRestore(true);
    CHECK(hub.restoreStatus(s2) == RestoreStatus::Applied && hub.restoreStatus(s1) == RestoreStatus::Superseded);
    CHECK(hub.clock().sampleIndex() == 20);
    hub.endBlock();
    CHECK(hub.restoreStatus(9999) == RestoreStatus::Pending && hub.restoreStatus(0) == RestoreStatus::Pending);
    CheckpointBlock huge = makeCp(1, 48000, 0, 1);
    huge.electricalCount = static_cast<std::uint32_t>(kMaxElectrical + 1);
    CHECK(hub.requestRestore(huge, 2) == 0);

    // Checkpoints captured under an old generation are not offered by save().
    SaveResult r;
    hub.save(r, microseconds(0));                       // posts a capture request
    CHECK(r.status == CheckpointStatus::Unavailable && !r.hasCheckpoint);
    const BlockControl& bc = hub.beginBlock(8);
    CHECK(bc.captureRequested);
    CheckpointBlock* t = hub.captureTarget();
    CHECK(t != nullptr && t->generation == 2 && t->sampleIndex == hub.clock().sampleIndex());
    t->electricalCount = 3; t->electrical[0] = t->electrical[1] = t->electrical[2] = 1;
    hub.publishCapture();
    CHECK(hub.captureTarget() == nullptr);  // request served
    hub.endBlock();
    hub.save(r, microseconds(0));
    CHECK(r.hasCheckpoint && r.status == CheckpointStatus::StaleFallback && r.checkpoint.generation == 2);
    hub.beginGeneration(0xBEEF);
    hub.save(r, microseconds(0));
    CHECK(!r.hasCheckpoint && r.status == CheckpointStatus::Unavailable);  // gen-2 checkpoint is old
    CHECK(r.config.generation == 3);                                         // configuration still present
    CheckpointBlock* none = hub.captureTarget();
    CHECK(none == nullptr);

    // A capture with invalid counts is discarded, not published.
    hub.beginBlock(0);
    CheckpointBlock* t2 = hub.captureTarget();
    if (t2) { t2->rngCount = 1000; hub.publishCapture(); }
    hub.endBlock();
    CHECK(hub.diagnostics().capturesDiscarded == 1);
}

// ------------------------------------------------------------------ stopped audio + persistence
const char* kScene = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"scene","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic scene","tags":[],"createdAt":"2026-10-30T12:00:00Z",
 "requiredFeatures":["CD-F20"],"blueprint":{"schemaVersion":1,"id":"example.rc","components":[{"id":"R1","modelId":"fixture.resistor"}]},
 "baseParameters":{"hpf.frequency":20.5},"macros":{},"timeline":{},"faults":[],"seed":42})";

void testStoppedAudioFallback() {
    auto hubp = std::make_unique<ControlHub>();
    ControlHub& hub = *hubp;
    hub.prepareClock(48000, 1000);
    const auto fp = floatParams(2);
    // Audio has never run (host stopped): configuration is still saved.
    CHECK(hub.stage(core::kParams[fp[0]].hostId, valueAt(fp[0], 0.5)));
    CHECK(hub.stage(core::kParams[fp[1]].hostId, valueAt(fp[1], 0.25)));
    const std::uint64_t rev = hub.commit();
    SaveResult r;
    const auto t0 = std::chrono::steady_clock::now();
    hub.save(r, microseconds(0));  // must not wait
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
    CHECK(r.status == CheckpointStatus::Unavailable && !r.hasCheckpoint && !r.checkpointMatchesConfig);
    CHECK(r.config.revision == rev && r.config.values[fp[0]] == valueAt(fp[0], 0.5) && r.config.values[fp[1]] == valueAt(fp[1], 0.25));
    // Bounded wait with silent audio also returns promptly with the same fallback.
    const auto t1 = std::chrono::steady_clock::now();
    hub.save(r, microseconds(2000));
    const auto waited = std::chrono::steady_clock::now() - t1;
    CHECK(waited >= std::chrono::microseconds(1500) && waited < std::chrono::milliseconds(500));
    CHECK(r.status == CheckpointStatus::Unavailable && r.config.revision == rev);

    // Configuration reaches a state document with no dynamic checkpoint (test-local adapter).
    state::StateDocument doc;
    std::string err;
    CHECK(state::loadState(kScene, doc, err));
    doc.baseParameters.clear();
    for (std::size_t idx : fp) doc.baseParameters.emplace_back(core::kParams[idx].id, r.config.values[idx]);
    CHECK(!r.hasCheckpoint && !doc.checkpoint.has_value());
    state::StateDocument back;
    CHECK(state::loadState(state::saveState(doc), back, err));
    CHECK(!back.checkpoint.has_value() && back.baseParameters.size() == 2);
    for (const auto& kv : back.baseParameters) {
        const core::ParamSpec* spec = core::findById(kv.first.c_str());
        CHECK(spec != nullptr && kv.second == r.config.values[static_cast<std::size_t>(spec - core::kParams)]);
    }

    // Audio ran once, then stopped: save returns the last coherent checkpoint, flagged stale.
    hub.beginBlock(480);
    hub.endBlock();  // adopt config
    hub.save(r, microseconds(0));  // post request
    hub.beginBlock(480);
    CheckpointBlock* t = hub.captureTarget();
    CHECK(t != nullptr);
    t->electricalCount = 1; t->electrical[0] = 3.5;
    hub.publishCapture();
    hub.endBlock();  // audio stops here
    hub.save(r, microseconds(1000));
    CHECK(r.hasCheckpoint && r.status == CheckpointStatus::StaleFallback);
    CHECK(r.checkpoint.sampleIndex == 480 && r.checkpoint.electrical[0] == 3.5 && r.checkpointMatchesConfig);
    // Newer configuration after the checkpoint: still saved; mismatch is reported, not hidden.
    hub.stage(core::kParams[fp[0]].hostId, valueAt(fp[0], 0.9));
    const std::uint64_t rev2 = hub.commit();
    hub.save(r, microseconds(0));
    CHECK(r.hasCheckpoint && !r.checkpointMatchesConfig && r.config.revision == rev2 && r.config.values[fp[0]] == valueAt(fp[0], 0.9));
}

// ------------------------------------------------------------------ handoff / reclamation
struct Tracked {
    static inline std::atomic<int> alive{0}, destroyed{0};
    static inline std::atomic<bool> destroyedOnAudio{false};
    explicit Tracked(std::uint64_t v) : value(v), check(~v) { alive.fetch_add(1); }
    ~Tracked() { if (tl_audio) destroyedOnAudio = true; alive.fetch_sub(1); destroyed.fetch_add(1); }
    std::uint64_t value, check;
};

void testHandoff() {
    using Slot = HandoffSlot<Tracked>;
    {
        Slot slot;
        CHECK(slot.take(1) == Slot::Take::None && slot.current() == nullptr);
        CHECK(slot.install(std::make_unique<Tracked>(1), 1) == nullptr);
        Slot::Take r;
        { AudioScope s; r = slot.take(1); }
        CHECK(r == Slot::Take::Installed && slot.current()->value == 1);
        CHECK(slot.install(std::make_unique<Tracked>(2), 1) == nullptr);
        { AudioScope s; r = slot.take(1); }
        CHECK(r == Slot::Take::Installed && slot.current()->value == 2);
        CHECK(Tracked::alive == 2 && Tracked::destroyed == 0);  // old payload retired, not destroyed on audio
        CHECK(slot.reclaim() == 1 && Tracked::alive == 1);

        // Old-generation rejection: never installed, retired for worker deletion.
        slot.install(std::make_unique<Tracked>(3), 5);
        { AudioScope s; r = slot.take(1); }
        CHECK(r == Slot::Take::RejectedStale && slot.current()->value == 2);
        CHECK(slot.reclaim() == 1 && Tracked::alive == 1);

        // Unconsumed install is replaced and handed back for worker-side deletion.
        slot.install(std::make_unique<Tracked>(4), 1);
        auto displaced = slot.install(std::make_unique<Tracked>(5), 1);
        CHECK(displaced != nullptr && displaced->payload->value == 4);
        displaced.reset();
        { AudioScope s; r = slot.take(1); }
        CHECK(r == Slot::Take::Installed && slot.current()->value == 5);

        // Retire ring pressure: audio defers instead of dropping or deleting.
        int deferred = 0, installed = 0;
        for (int i = 0; i < 20; ++i) {
            slot.install(std::make_unique<Tracked>(100 + i), 1);
            { AudioScope s; r = slot.take(1); }
            if (r == Slot::Take::Deferred) ++deferred;
            if (r == Slot::Take::Installed) ++installed;
        }
        CHECK(deferred > 0 && installed >= 1);
        CHECK(slot.reclaim() > 0);
        { AudioScope s; r = slot.take(1); }
        CHECK(r == Slot::Take::Installed);  // pending node delivered after reclamation freed space
        slot.reclaim();
    }
    CHECK(Tracked::alive == 0);
    CHECK(!Tracked::destroyedOnAudio.load());
    CHECK(g_audioAllocs.load() == 0 && g_audioFrees.load() == 0);
}

void testTrapPositiveControl() {
    const auto a0 = g_audioAllocs.load(), f0 = g_audioFrees.load();
    { AudioScope s; int* p = new int(7); delete p; }
    CHECK(g_audioAllocs.load() == a0 + 1 && g_audioFrees.load() == f0 + 1);
    { int* q = new int(1); delete q; }  // outside the scope: not counted
    CHECK(g_audioAllocs.load() == a0 + 1);
    g_audioAllocs = 0; g_audioFrees = 0;  // reset for the real checks
}

// ------------------------------------------------------------------ concurrency
struct Payload { std::uint64_t v, inv; Payload(std::uint64_t x) : v(x), inv(~x) {} };

void testConcurrent() {
    auto hubp = std::make_unique<ControlHub>();
    ControlHub& hub = *hubp;
    hub.prepareClock(48000, 1000);
    const auto fp = floatParams(3);
    const std::size_t ia = fp[0], ib = fp[1];
    HandoffSlot<Payload> slot;

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> bad{0}, blocks{0}, events{0}, fresh{0}, stale{0}, unavail{0}, saves{0}, noCfgSeen{0};
    std::atomic<std::uint64_t> rApplied{0}, rStale{0}, rOther{0}, installs{0}, takes{0}, zeroBlocks{0}, captures{0};
    const std::uint32_t sizes[] = {0, 1, 17, 64, 127, 512, 8192, 0, 33};

    std::thread audio([&] {
        std::uint64_t mark = 0, lastRev = 0, lastGen = 0, rng = 88172645463325252ull;
        std::uint32_t spin = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            {
                AudioScope scope;
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                const std::uint32_t frames = sizes[rng % 9];
                const std::uint64_t idxBefore = hub.clock().sampleIndex();
                const BlockControl& b = hub.beginBlock(frames);
                const ConfigBlock& c = hub.currentConfig();
                if (b.generationChanged) lastRev = 0;
                if (c.revision != 0) {
                    if (std::fabs(fracOf(ia, c.values[ia]) - fracOf(ib, c.values[ib])) > 1e-9) ++bad;  // torn configuration
                    if (c.generation != hub.audioGeneration()) ++bad;
                    if (c.generation == lastGen && c.revision < lastRev) ++bad;  // revision regression
                    lastGen = c.generation; lastRev = c.revision;
                } else {
                    ++noCfgSeen;
                }
                for (std::uint32_t i = 0; i < b.eventCount; ++i) {
                    if (b.events[i].generation != hub.audioGeneration() || b.events[i].revision < c.revision) ++bad;
                }
                events += b.eventCount;
                if (b.restore != nullptr) {
                    const CheckpointBlock& cp = *b.restore;
                    bool ok = cp.countsValid() && cp.electricalCount == 64 && cp.thermalCount == 32 && cp.rngCount == 1;
                    for (std::uint32_t i = 0; ok && i < cp.electricalCount; ++i) ok = cp.electrical[i] == static_cast<double>(cp.rng[0]);
                    for (std::uint32_t i = 0; ok && i < cp.thermalCount; ++i) ok = cp.thermal[i] == static_cast<double>(cp.rng[0]);
                    if (!ok) ++bad;
                    mark = cp.rng[0];
                    hub.commitRestore(true);
                    if (hub.clock().sampleIndex() != cp.sampleIndex) ++bad;
                }
                if (CheckpointBlock* t = hub.captureTarget()) {
                    if (t->sampleIndex != hub.clock().sampleIndex() || t->generation != hub.audioGeneration()) ++bad;
                    t->electricalCount = 64; t->thermalCount = 32; t->rngCount = 1;
                    for (std::uint32_t i = 0; i < 64; ++i) t->electrical[i] = static_cast<double>(mark);
                    for (std::uint32_t i = 0; i < 32; ++i) t->thermal[i] = static_cast<double>(mark);
                    t->rng[0] = mark;
                    hub.publishCapture();
                    ++captures;
                }
                const auto tk = slot.take(hub.audioGeneration());
                if (tk == HandoffSlot<Payload>::Take::Installed) ++takes;
                if (Payload* p = slot.current()) { if (p->inv != ~p->v) ++bad; }
                ++mark;
                hub.endBlock();
                if (frames == 0) { ++zeroBlocks; if (hub.clock().sampleIndex() != idxBefore && !b.restore) ++bad; }
                ++blocks;
            }
            if ((++spin & 63u) == 0) std::this_thread::yield();
        }
    });

    std::thread ui([&] {
        for (std::uint64_t k = 1; !stop.load(); ++k) {
            const double f = static_cast<double>(k % 1000) / 1000.0;
            hub.stage(core::kParams[ia].hostId, valueAt(ia, f));
            hub.stage(core::kParams[ib].hostId, valueAt(ib, f));
            hub.commit();
            for (int j = 0; j < 3; ++j) hub.sendParam(core::kParams[fp[2]].hostId, valueAt(fp[2], f));  // single-param automation
            if (k % 100 == 0) hub.beginGeneration(k);
            std::this_thread::sleep_for(microseconds(40));
        }
    });

    auto saver = [&](microseconds timeout, bool restore) {
        std::uint64_t n = 0;
        while (!stop.load()) {
            SaveResult r;
            hub.save(r, timeout);
            ++saves;
            if (std::fabs(fracOf(ia, r.config.values[ia]) - fracOf(ib, r.config.values[ib])) > 1e-9 && r.config.revision > 1) ++bad;
            switch (r.status) {
                case CheckpointStatus::Fresh: ++fresh; break;
                case CheckpointStatus::StaleFallback: ++stale; break;
                case CheckpointStatus::Unavailable: ++unavail; break;
            }
            if (r.hasCheckpoint) {
                const CheckpointBlock& cp = r.checkpoint;
                bool ok = cp.electricalCount == 64 && cp.thermalCount == 32 && cp.rngCount == 1 && cp.generation <= hub.generation();
                for (std::uint32_t i = 0; ok && i < 64; ++i) ok = cp.electrical[i] == static_cast<double>(cp.rng[0]);
                for (std::uint32_t i = 0; ok && i < 32; ++i) ok = cp.thermal[i] == static_cast<double>(cp.rng[0]);
                if (!ok) ++bad;
                if (restore && (++n % 3) == 0) {
                    const std::uint64_t seq = hub.requestRestore(cp, cp.generation);
                    RestoreStatus st = RestoreStatus::Pending;
                    for (int w = 0; w < 100 && st == RestoreStatus::Pending; ++w) {
                        std::this_thread::sleep_for(microseconds(50));
                        st = hub.restoreStatus(seq);
                    }
                    if (st == RestoreStatus::Applied) ++rApplied;
                    else if (st == RestoreStatus::RejectedStaleGeneration) ++rStale;
                    else ++rOther;
                }
            }
            std::this_thread::sleep_for(microseconds(30));
        }
    };
    std::thread w1(saver, microseconds(5000), true);
    std::thread w2(saver, microseconds(0), false);

    std::thread prep([&] {  // worker: install prepared handles, reclaim retired ones
        std::uint64_t k = 0;
        while (!stop.load()) {
            slot.install(std::make_unique<Payload>(++k), hub.generation());
            ++installs;
            slot.reclaim();
            std::this_thread::sleep_for(microseconds(100));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop = true;
    audio.join(); ui.join(); w1.join(); w2.join(); prep.join();

    std::printf("concurrent: blocks=%llu zero=%llu events=%llu captures=%llu saves=%llu fresh=%llu stale=%llu unavail=%llu "
                "restore applied=%llu staleGen=%llu other=%llu installs=%llu takes=%llu allocsOnAudio=%llu\n",
                (unsigned long long)blocks, (unsigned long long)zeroBlocks, (unsigned long long)events,
                (unsigned long long)captures, (unsigned long long)saves, (unsigned long long)fresh,
                (unsigned long long)stale, (unsigned long long)unavail, (unsigned long long)rApplied,
                (unsigned long long)rStale, (unsigned long long)rOther, (unsigned long long)installs,
                (unsigned long long)takes, (unsigned long long)g_audioAllocs.load());
    CHECK(bad.load() == 0);
    CHECK(blocks > 100 && zeroBlocks > 0 && captures > 0 && saves > 0);
    CHECK(fresh > 0);       // audio answered a save within its bound at least once
    CHECK(unavail + stale + fresh == saves.load());
    CHECK(rApplied > 0);
    CHECK(takes > 0 && installs > 0);
    CHECK(g_audioAllocs.load() == 0 && g_audioFrees.load() == 0);
}

}  // namespace

int main() {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free &&
                  std::atomic<void*>::is_always_lock_free, "lock-free atomics required");
    testTrapPositiveControl();
    testRing();
    testLatestSlot();
    testClockPartitions();
    testHubPartitions();
    testQueuePressure();
    testGenerations();
    testStoppedAudioFallback();
    testHandoff();
    testConcurrent();
    std::printf("control queue tests: failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
