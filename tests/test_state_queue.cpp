// P04 queue/snapshot interface tests: SPSC ring, parameter/config transaction queue (overflow,
// coalescing, note-off guarantee, zero-frame/partitioned slices), generation pool (stale
// rejection, ack-gated reclamation) and snapshot exchange (concurrent saves while audio
// publishes, stopped audio, incompatible Fs/model/layout fallback). Synthetic data only.
// Multi-threaded sections use std::thread; run under -fsanitize=thread for race checking.
#include "core/ParameterRegistry.h"
#include "state/AbSlots.h"
#include "state/RtQueue.h"
#include "state/SnapshotExchange.h"
#include "state/TransactionQueue.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <map>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

// Allocation probe: shared UX-09 trap (tests/support/RtTrap), counts this thread only.
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::state;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();
constexpr std::uint32_t kMix = 1007, kHpf = 1011, kInGain = 1005, kAb = 1017;

// ---------------- SPSC ring ----------------
void spscBasics() {
    SpscQueue<int> q(5);
    CHECK(q.capacity() == 8);
    int v = 0;
    CHECK(!q.tryPop(v));
    for (int i = 0; i < 8; ++i) CHECK(q.tryPush(i));
    CHECK(!q.tryPush(99));
    CHECK(q.sizeApprox() == 8);
    for (int i = 0; i < 8; ++i) { CHECK(q.tryPop(v) && v == i); }
    CHECK(!q.tryPop(v));
    // keepFree reserves the last slots for a higher-priority producer class.
    for (int i = 0; i < 6; ++i) CHECK(q.tryPush(i, 2));
    CHECK(!q.tryPush(6, 2));
    CHECK(q.tryPush(6) && q.tryPush(7) && !q.tryPush(8));
    CHECK(!q.tryPush(0, 8));
    bool threw = false;
    try { SpscQueue<int> bad(0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void spscStress() {
    constexpr std::uint64_t kN = 1000000;
    SpscQueue<std::uint64_t> q(1024);
    std::atomic<bool> orderOk{true};
    std::uint64_t received = 0;
    std::thread consumer([&] {
        std::uint64_t expect = 0, v = 0;
        while (expect < kN) {
            if (q.tryPop(v)) { if (v != expect) orderOk = false; ++expect; }
            else std::this_thread::yield();
        }
        received = expect;
    });
    for (std::uint64_t i = 0; i < kN;) { if (q.tryPush(i)) ++i; else std::this_thread::yield(); }
    consumer.join();
    CHECK(orderOk.load());
    CHECK(received == kN);
    std::printf("SPSC stress: %llu events, order preserved, no loss\n", (unsigned long long)received);
}

// ---------------- Transaction queue ----------------
void rejections() {
    TransactionQueue q(256);
    CHECK(q.push(TransactionEvent::param(999, .5, 0)) == PushResult::Rejected);   // unknown host id
    CHECK(q.push(TransactionEvent::param(2000, .5, 0)) == PushResult::Rejected);
    for (double bad : {kNan, kInf, -kInf, -0.1, 1.1, 1e300})
        CHECK(q.push(TransactionEvent::param(kMix, bad, 0)) == PushResult::Rejected);
    CHECK(q.push(TransactionEvent::config(0, 0, 0)) == PushResult::Rejected);
    CHECK(q.push(TransactionEvent::config(kMaxGeneration + 1, 0, 0)) == PushResult::Rejected);
    CHECK(q.push(TransactionEvent::config(1, 256, 0)) == PushResult::Rejected);
    CHECK(q.push(TransactionEvent::noteOff(16, 1, 0)) == PushResult::Rejected);
    CHECK(q.push(TransactionEvent::noteOff(0, 128, 0)) == PushResult::Rejected);
    CHECK(q.push(TransactionEvent::allNotesOff(17, 0)) == PushResult::Rejected);
    CHECK(q.diagnostics().rejected == 14);
    CHECK(q.drainSlice(64).empty());
    CHECK(q.push(TransactionEvent::param(kMix, 0.0, 0)) == PushResult::Queued);
    CHECK(q.push(TransactionEvent::param(kMix, 1.0, 0)) == PushResult::Queued);
    const auto s = q.drainSlice(64);
    CHECK(s.size() == 2 && s[1].normalized == 1.0 && s[0].registryIndex == std::uint32_t(core::findByHostId(kMix) - core::kParams));
    bool threw = false;
    try { TransactionQueue tiny(64); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void sortedClampedAndZeroFrame() {
    TransactionQueue q;
    q.push(TransactionEvent::param(kMix, .3, 50));
    q.push(TransactionEvent::param(kHpf, .1, 10));
    q.push(TransactionEvent::noteOff(0, 60, 10));
    q.push(TransactionEvent::param(kMix, .4, 10));
    q.push(TransactionEvent::param(kInGain, .9, 1000000));
    auto s = q.drainSlice(64);
    CHECK(s.size() == 5);
    for (std::size_t i = 1; i < s.size(); ++i) CHECK(s[i - 1].sampleOffset <= s[i].sampleOffset);
    // Stable: the three offset-10 events keep arrival order.
    CHECK(s[0].hostId == kHpf && s[1].kind == EventKind::NoteOff && s[2].hostId == kMix && s[2].normalized == .4);
    CHECK(s[3].normalized == .3 && s[3].sampleOffset == 50);
    CHECK(s[4].hostId == kInGain && s[4].sampleOffset == 63);
    // Zero-frame slice: events still apply (offset 0), no clock to advance.
    q.push(TransactionEvent::param(kMix, .7, 40));
    q.push(TransactionEvent::config(3, 1, 9));
    s = q.drainSlice(0);
    CHECK(s.size() == 2 && s[0].sampleOffset == 0 && s[1].sampleOffset == 0 && s[1].generation == 3);
    CHECK(q.drainSlice(0).empty());
}

// Slice overflow: more than the timed region -> latest value per parameter, sorted, <= 4096.
void sliceOverflowCoalescing() {
    TransactionQueue q(16384);
    std::map<std::uint32_t, double> latest;
    const std::uint32_t ids[] = {kMix, kHpf, kInGain, kAb};
    for (int i = 0; i < 9000; ++i) {
        const std::uint32_t id = ids[i % 4];
        const double v = double(i) / 9000.0;
        CHECK(q.push(TransactionEvent::param(id, v, std::uint32_t(i % 512))) == PushResult::Queued);
        latest[id] = v;
    }
    for (int n = 0; n < 300; ++n) CHECK(q.push(TransactionEvent::noteOff(0, std::uint8_t(n % 128), 5)) == PushResult::Queued);
    const auto s = q.drainSlice(512);
    CHECK(s.size() <= kSliceEventCapacity);
    std::map<std::uint32_t, double> applied;
    std::size_t notes = 0, alls = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i) CHECK(s[i - 1].sampleOffset <= s[i].sampleOffset);
        if (s[i].kind == EventKind::Param) applied[s[i].hostId] = s[i].normalized;  // apply in order
        if (s[i].kind == EventKind::NoteOff) ++notes;
        if (s[i].kind == EventKind::AllNotesOff) { ++alls; CHECK(s[i].channel == kOmniChannel && s[i].coalesced); }
    }
    CHECK(applied == latest);              // final parameter state exact after coalescing
    CHECK(notes >= kNoteReserve);           // reserved note-off slots honoured
    CHECK(notes + alls >= 1 && alls == 1);  // overflowed note-offs became one AllNotesOff
    CHECK(s.back().kind == EventKind::AllNotesOff);
    const QueueDiagnostics d = q.diagnostics();
    CHECK(d.sliceCoalesced == 9000 - kTimedCapacity);
    CHECK(d.forcedAllNotesOff == 1);
    std::printf("slice overflow: 9300 pending -> %zu events (%zu note-offs + AllNotesOff), sliceCoalesced=%llu\n",
                s.size(), notes, (unsigned long long)d.sliceCoalesced);
}

// Ring full (audio stopped): producer never blocks, keeps latest value, notes still get through.
void ringFullAudioStopped() {
    TransactionQueue q(256);
    std::size_t queued = 0, coalesced = 0;
    for (int i = 0; i < 1000; ++i) {
        const PushResult r = q.push(TransactionEvent::param(i % 2 ? kMix : kHpf, double(i) / 1000.0, 0));
        (r == PushResult::Queued ? queued : coalesced) += 1;
    }
    CHECK(queued == 256 - kRingNoteReserve && coalesced == 1000 - queued);
    CHECK(q.push(TransactionEvent::config(5, 2, 0)) == PushResult::Coalesced);
    CHECK(q.push(TransactionEvent::config(4, 1, 0)) == PushResult::Coalesced);  // older: newest kept
    std::size_t notesQueued = 0;
    for (int n = 0; n < 100; ++n) notesQueued += q.push(TransactionEvent::noteOff(1, std::uint8_t(n), 0)) == PushResult::Queued;
    CHECK(notesQueued == kRingNoteReserve);  // reserve slots usable only by note-offs
    CHECK(q.hasPending() && !q.flushPending());
    CHECK(q.diagnostics().forcedAllNotesOff == 1);
    // Audio resumes.
    std::map<std::uint32_t, double> applied;
    std::size_t notes = 0, alls = 0;
    std::uint64_t installedGen = 0;
    for (int slice = 0; slice < 8; ++slice) {
        for (const auto& e : q.drainSlice(128)) {
            if (e.kind == EventKind::Param) applied[e.hostId] = e.normalized;
            if (e.kind == EventKind::NoteOff) ++notes;
            if (e.kind == EventKind::AllNotesOff) ++alls;
            if (e.kind == EventKind::ConfigPublish) installedGen = e.generation;
        }
        q.flushPending();  // message-thread timer
    }
    CHECK(!q.hasPending());
    CHECK(applied[kMix] == 999.0 / 1000.0 && applied[kHpf] == 998.0 / 1000.0);
    CHECK(notes == kRingNoteReserve && alls == 1);
    CHECK(installedGen == 5);
    CHECK(q.diagnostics().flushed == 4);  // all-notes-off, config, two parameters
}

// Multi-thread stress: 1e6 events, per-parameter order preserved, exact accounting of losses.
void transactionStress() {
    constexpr int kN = 1000000;
    const std::uint32_t ids[] = {kMix, kHpf, kInGain, kAb};
    TransactionQueue q(4096);
    std::atomic<bool> producerDone{false};
    std::atomic<bool> orderOk{true};
    std::atomic<std::uint64_t> received{0};
    std::array<double, 4> lastSeen{-1, -1, -1, -1};
    std::uint64_t queued = 0;
    std::thread audio([&] {
        std::uint64_t got = 0;
        int idleAfterDone = 0;
        std::uint32_t frames[] = {0, 1, 17, 64, 127, 512, 8192};
        for (int k = 0; idleAfterDone < 1000; ++k) {
            const auto s = q.drainSlice(frames[k % 7]);
            for (const auto& e : s) {
                int j = 0;
                while (ids[j] != e.hostId) ++j;
                if (!(e.normalized > lastSeen[j])) orderOk = false;  // strictly increasing per parameter
                lastSeen[j] = e.normalized;
                if (!e.coalesced) ++got;
            }
            if (s.empty() && producerDone.load(std::memory_order_acquire)) ++idleAfterDone;
            else if (s.empty()) std::this_thread::yield();
        }
        received = got;
    });
    for (int i = 0; i < kN; ++i) {
        const PushResult r = q.push(TransactionEvent::param(ids[i % 4], double(i + 1) / double(kN), 0));
        if (r == PushResult::Queued) ++queued;
        if ((i & 1023) == 0) std::this_thread::yield();
    }
    while (!q.flushPending()) std::this_thread::yield();
    producerDone.store(true, std::memory_order_release);
    audio.join();
    const QueueDiagnostics d = q.diagnostics();
    CHECK(orderOk.load());
    for (int j = 0; j < 4; ++j) CHECK(lastSeen[std::size_t(j)] == double(kN - 3 + j) / double(kN));  // final value exact
    // Every queued event is delivered unless counted as slice-coalesced; nothing else is lost.
    CHECK(received.load() + d.sliceCoalesced >= queued);
    CHECK(received.load() + d.sliceCoalesced <= queued + d.flushed);
    CHECK(queued + d.ringCoalesced == std::uint64_t(kN));
    std::printf("transaction stress: %d pushed, %llu queued, %llu ring-coalesced, %llu flushed, %llu slice-coalesced, %llu delivered timed\n",
                kN, (unsigned long long)queued, (unsigned long long)d.ringCoalesced, (unsigned long long)d.flushed,
                (unsigned long long)d.sliceCoalesced, (unsigned long long)received.load());
}

// Block partitions {1,17,64,127,512,8192} plus zero-frame calls give identical applied timelines.
void partitionAgreement() {
    constexpr std::uint32_t kTotal = 16384;
    struct Ev { std::uint32_t at; double v; };
    std::vector<Ev> evs;
    for (std::uint32_t i = 0; i < 400; ++i) evs.push_back({(i * 2654435761u) % kTotal, double(i) / 400.0});
    std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
    std::vector<double> reference;
    for (std::uint32_t block : {1u, 17u, 64u, 127u, 512u, 8192u}) {
        TransactionQueue q;
        std::vector<double> timeline(kTotal, -1);
        double value = -1;
        std::size_t next = 0;
        for (std::uint32_t start = 0; start < kTotal;) {
            const std::uint32_t n = std::min(block, kTotal - start);
            q.drainSlice(0);  // interleaved zero-frame call with nothing pending
            while (next < evs.size() && evs[next].at < start + n) {
                q.push(TransactionEvent::param(kMix, evs[next].v, evs[next].at - start));
                ++next;
            }
            const auto s = q.drainSlice(n);
            std::size_t k = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                while (k < s.size() && s[k].sampleOffset == i) value = s[k++].normalized;
                timeline[start + i] = value;
            }
            start += n;
        }
        if (reference.empty()) reference = timeline;
        CHECK(timeline == reference);
    }
}

// ---------------- Generation pool ----------------
struct Config { std::uint64_t generation = 0; std::array<double, 64> data{}; };

void generationPoolBasics() {
    GenerationPool<Config, 3> pool;
    CHECK(pool.installLatest() == nullptr);
    const std::uint32_t s0 = pool.acquireFree();
    CHECK(s0 != pool.kNone);
    pool.payload(s0)->generation = 1;
    CHECK(pool.publish(s0, 1));
    CHECK(!pool.publish(s0, 2));  // already published, not writable
    const Config* c = pool.installLatest();
    CHECK(c && c->generation == 1 && pool.acknowledgedGeneration() == 1);
    const std::uint32_t s1 = pool.acquireFree();
    pool.payload(s1)->generation = 2;
    CHECK(!pool.publish(s1, 1));  // old generation rejected
    CHECK(!pool.publish(s1, 0));
    CHECK(!pool.publish(s1, kMaxGeneration + 1));
    CHECK(pool.publish(s1, 2));
    const std::uint32_t s2 = pool.acquireFree();
    CHECK(pool.acquireFree() == pool.kNone);  // all slots busy
    CHECK(pool.reclaim() == 0);               // gen 1 still installed (ack 1)
    pool.payload(s2)->generation = 3;
    CHECK(pool.publish(s2, 3));
    // Audio installs gen 3 via a queued event; a stale event for gen 2 afterwards is rejected
    // and leaves the live configuration unchanged.
    c = pool.tryInstall(3, s2);
    CHECK(c && c->generation == 3);
    CHECK(pool.tryInstall(2, s1) == nullptr);
    CHECK(pool.tryInstall(3, s2) == nullptr);   // duplicate
    CHECK(pool.tryInstall(4, s2) == nullptr);   // unknown generation for slot
    CHECK(pool.tryInstall(9, 7) == nullptr);    // bad handle
    CHECK(pool.current()->generation == 3 && pool.installedGenerationAudio() == 3);
    CHECK(pool.reclaim() == 2);                 // gens 1 and 2 reclaimed only after ack of 3
    CHECK(pool.acquireFree() != pool.kNone);
}

void generationPoolStress() {
    GenerationPool<Config, 4> pool;
    std::atomic<bool> done{false}, coherent{true};
    std::atomic<std::uint64_t> installs{0};
    std::thread audio([&] {
        std::uint64_t last = 0, n = 0;
        while (!done.load(std::memory_order_acquire)) {
            const Config* c = pool.installLatest();
            if (!c) continue;
            if (c->generation < last) coherent = false;
            for (double d : c->data) if (d != double(c->generation)) { coherent = false; break; }
            if (c->generation != last) ++n;
            last = c->generation;
        }
        installs = n;
    });
    std::uint64_t gen = 0;
    while (gen < 100000) {
        pool.reclaim();
        const std::uint32_t s = pool.acquireFree();
        if (s == pool.kNone) { std::this_thread::yield(); continue; }
        Config* p = pool.payload(s);
        ++gen;
        p->generation = gen;
        p->data.fill(double(gen));
        CHECK(pool.publish(s, gen));
    }
    done.store(true, std::memory_order_release);
    audio.join();
    CHECK(coherent.load());
    CHECK(installs.load() > 0);
    std::printf("generation pool stress: 100000 publishes, %llu distinct installs, payloads coherent\n",
                (unsigned long long)installs.load());
}

void configThroughQueue() {
    GenerationPool<Config, 4> pool;
    TransactionQueue q;
    std::uint32_t slots[3];
    for (std::uint64_t g = 1; g <= 3; ++g) {
        slots[g - 1] = pool.acquireFree();
        pool.payload(slots[g - 1])->generation = g;
        CHECK(pool.publish(slots[g - 1], g));
    }
    q.push(TransactionEvent::config(2, slots[1], 0));
    q.push(TransactionEvent::config(1, slots[0], 5));  // reordered/stale publish
    q.push(TransactionEvent::config(3, slots[2], 9));
    std::vector<std::uint64_t> installedSeq;
    for (const auto& e : q.drainSlice(16))
        if (e.kind == EventKind::ConfigPublish && pool.tryInstall(e.generation, e.handleIndex)) installedSeq.push_back(e.generation);
    CHECK((installedSeq == std::vector<std::uint64_t>{2, 3}));
    CHECK(pool.current()->generation == 3);
}

// ---------------- Snapshot exchange ----------------
constexpr std::uint32_t kDoubles = 512;
SnapshotIdentity identity48k() {
    SnapshotIdentity id;
    id.layout = {0x53594E31u /* synthetic fixture layout */, 1, kDoubles * sizeof(double)};
    id.sampleRate = 48000;
    id.oversampling = 2;
    id.modelId = 0xC1DCull;
    return id;
}

void fill(std::span<std::byte> p, std::uint64_t seq) {
    for (std::uint32_t i = 0; i < kDoubles; ++i) {
        const double v = double(seq) * 1000.0 + i;
        std::memcpy(p.data() + i * sizeof(double), &v, sizeof v);
    }
}
bool coherentSnapshot(const SavedSnapshot& s) {
    if (!s.valid || s.payload.size() != kDoubles * sizeof(double)) return false;
    const std::uint64_t seq = s.header.sampleIndex / 64;
    if (s.header.configRevision != seq || s.header.clockRemainder != double(seq % 48) / 48.0) return false;
    for (std::uint32_t i = 0; i < kDoubles; ++i) {
        double v;
        std::memcpy(&v, s.payload.data() + i * sizeof(double), sizeof v);
        if (v != double(seq) * 1000.0 + i) return false;
    }
    return true;
}

void snapshotNoCheckpointAndRequests() {
    SnapshotExchange x(identity48k());
    HostSaveResult r = hostSave(x, "{\"config\":1}");
    CHECK(r.decision == RestoreDecision::NoCheckpoint && !r.snapshot.valid && r.configuration == "{\"config\":1}");
    std::string reason;
    CHECK(decideSnapshotRestore(r.snapshot, identity48k(), reason) == RestoreDecision::NoCheckpoint);
    CHECK(!x.captureRequested());
    const std::uint64_t token = x.requestCapture();
    CHECK(x.captureRequested());
    fill(x.writePayload(), 1);
    x.publish(64, 1.0 / 48.0, 1);
    CHECK(!x.captureRequested());
    r = hostSave(x, "{}");
    CHECK(r.decision == RestoreDecision::Restore && coherentSnapshot(r.snapshot) && r.snapshot.header.servedRequest >= token);
    // Same snapshot readable repeatedly (front kept) when nothing new is published.
    SavedSnapshot again;
    x.readLatest(again);
    CHECK(coherentSnapshot(again) && again.header.publishSequence == 1);
    bool threw = false;
    try { SnapshotIdentity bad = identity48k(); bad.layout.byteSize = 0; SnapshotExchange y(bad); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void snapshotIncompatibleFallback() {
    SnapshotExchange x(identity48k());
    fill(x.writePayload(), 7);
    x.publish(7 * 64, double(7 % 48) / 48.0, 7);
    SavedSnapshot saved;
    x.readLatest(saved);
    CHECK(coherentSnapshot(saved));
    std::vector<std::byte> live(kDoubles * sizeof(double), std::byte{0x5A});
    const std::vector<std::byte> before = live;
    auto expectReset = [&](SnapshotIdentity rt, const char* what) {
        std::string reason;
        CHECK(restoreSnapshot(saved, rt, live, reason) == RestoreDecision::ResetWarm);
        CHECK(live == before);  // live state untouched on rejection
        CHECK(reason.find(what) != std::string::npos && reason.find("configuration only") != std::string::npos);
    };
    SnapshotIdentity rt = identity48k(); rt.sampleRate = 44100; expectReset(rt, "sample rate");
    rt = identity48k(); rt.oversampling = 4; expectReset(rt, "oversampling");
    rt = identity48k(); rt.modelId = 0xBEEF; expectReset(rt, "model");
    rt = identity48k(); rt.layout.layoutId = 2; expectReset(rt, "layout id");
    rt = identity48k(); rt.layout.version = 2; expectReset(rt, "layout version");
    rt = identity48k(); rt.layout.byteSize += 8; expectReset(rt, "layout size");
    std::string reason;
    std::vector<std::byte> wrongSize(16);
    CHECK(restoreSnapshot(saved, identity48k(), wrongSize, reason) == RestoreDecision::ResetWarm);
    CHECK(restoreSnapshot(saved, identity48k(), live, reason) == RestoreDecision::Restore && live == saved.payload);
}

void snapshotConcurrentSavesAndStoppedAudio() {
    SnapshotExchange x(identity48k());
    constexpr std::uint64_t kPublishes = 20000;
    std::atomic<bool> audioRunning{true}, allCoherent{true};
    std::atomic<std::uint64_t> reads{0};
    std::thread audio([&] {
        for (std::uint64_t seq = 1; seq <= kPublishes; ++seq) {
            // Block boundary: capture only when requested, plus a periodic publish.
            if (x.captureRequested() || seq % 4 == 0 || seq == kPublishes) {
                fill(x.writePayload(), seq);
                x.publish(seq * 64, double(seq % 48) / 48.0, seq);
            }
        }
        audioRunning.store(false, std::memory_order_release);
    });
    std::vector<std::thread> savers;
    for (int t = 0; t < 3; ++t) {
        savers.emplace_back([&, t] {
            std::uint64_t lastSeq = 0;
            std::vector<std::byte> live(kDoubles * sizeof(double));
            while (audioRunning.load(std::memory_order_acquire)) {
                if (t == 0) x.requestCapture();
                HostSaveResult r = hostSave(x, "{}");
                if (r.decision == RestoreDecision::Restore) {
                    if (!coherentSnapshot(r.snapshot) || r.snapshot.header.publishSequence < lastSeq) allCoherent = false;
                    lastSeq = r.snapshot.header.publishSequence;
                    std::string reason;  // concurrent restore into a worker-side buffer
                    if (restoreSnapshot(r.snapshot, x.identity(), live, reason) != RestoreDecision::Restore || live != r.snapshot.payload)
                        allCoherent = false;
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    audio.join();
    for (auto& s : savers) s.join();
    CHECK(allCoherent.load());
    // Audio is now stopped: a host save must return the last coherent snapshot immediately.
    const auto t0 = std::chrono::steady_clock::now();
    HostSaveResult r = hostSave(x, "{}");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.decision == RestoreDecision::Restore && coherentSnapshot(r.snapshot));
    CHECK(r.snapshot.header.sampleIndex == kPublishes * 64);
    const std::uint64_t pending = x.requestCapture();
    r = hostSave(x, "{}");
    CHECK(r.snapshot.header.servedRequest < pending && r.snapshot.header.sampleIndex == kPublishes * 64);
    std::printf("snapshot exchange: %llu concurrent saves coherent; stopped-audio save %.3f ms\n",
                (unsigned long long)reads.load(), ms);
}

// Audio-side entry points perform no heap allocation (overflow, install, publish, mix).
void audioPathsDoNotAllocate() {
    TransactionQueue q(8192);
    GenerationPool<Config, 4> pool;
    SnapshotExchange x(identity48k());
    AbMixer mixer;
    CHECK(mixer.prepare(48000));
    const std::uint32_t s = pool.acquireFree();
    CHECK(pool.publish(s, 1));
    for (int i = 0; i < 8000; ++i) q.push(TransactionEvent::param(i % 3 ? kMix : kHpf, double(i % 100) / 100.0, std::uint32_t(8000 - i)));
    for (int i = 0; i < 300; ++i) q.push(TransactionEvent::noteOff(0, 1, 3));
    q.push(TransactionEvent::config(1, s, 2));
    mixer.setPosition(.5); mixer.swap();
    std::vector<double> a(512, .25), b(512, -.5), out(512);
    {   // positive control: the trap must see this allocation
        cd::test::RtTrapScope ctl;
        {
            std::vector<int> selfTest(3);
#if defined(__GNUC__)
            asm volatile("" : : "r"(selfTest.data()) : "memory");  // keep clang from eliding the new/delete pair
#endif
            (void)selfTest;
        }
        CHECK(ctl.counts().allocations == 1);
    }
    std::size_t n = 0;
    cd::test::RtTrapCounts audioCounts;
    {
    cd::test::RtTrapScope trap;
    for (std::uint32_t frames : {512u, 0u, 1u, 8192u}) {
        for (const auto& e : q.drainSlice(frames)) {
            ++n;
            if (e.kind == EventKind::ConfigPublish) (void)pool.tryInstall(e.generation, e.handleIndex);
        }
        (void)pool.installLatest();
        (void)x.captureRequested();
        fill(x.writePayload(), 1);
        x.publish(64, 1.0 / 48.0, 1);
        mixer.beginBlock();
        mixer.mix(a.data(), b.data(), out.data(), std::min<std::uint32_t>(frames, 512));
    }
    audioCounts = trap.counts();
    }
    CHECK(audioCounts.allocations == 0 && audioCounts.deallocations == 0);
    CHECK(n > 0 && pool.current() != nullptr);
    std::printf("audio paths: %zu events drained under overflow, 0 heap allocations on the audio side\n", n);
}

}  // namespace

int main() {
    spscBasics();
    spscStress();
    rejections();
    sortedClampedAndZeroFrame();
    sliceOverflowCoalescing();
    ringFullAudioStopped();
    transactionStress();
    partitionAgreement();
    generationPoolBasics();
    generationPoolStress();
    configThroughQueue();
    snapshotNoCheckpointAndRequests();
    snapshotIncompatibleFallback();
    snapshotConcurrentSavesAndStoppedAudio();
    audioPathsDoNotAllocate();
    if (failures) { std::printf("state_queue: %d failure(s)\n", failures); return 1; }
    std::puts("state_queue: all checks passed (synthetic fixtures)");
    return 0;
}
