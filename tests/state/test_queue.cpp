// SPSC parameter transaction queue + snapshot publisher (CD-F09, UX-09). Synthetic data only.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <latch>
#include <new>
#include <string>
#include <thread>
#include "state/ParamTransactionQueue.h"
#include "check.h"

// Count heap allocations so the audio-thread path can be proven allocation-free.
static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_count{false};
void* operator new(std::size_t n) {
    if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace cd;
using namespace cd::state;

namespace cd::state {
// Expose the actual production drain phases for one deterministic interleaving.
// The production callback contains no synchronization hook or test-only branch.
struct ParamTransactionQueueTestAccess {
    static DrainResult ring(ParamTransactionQueue& queue, core::ParamValues& values) {
        DrainResult result;
        queue.drainRing(values, result, nullptr, 0);
        return result;
    }
    static DrainResult coalesced(ParamTransactionQueue& queue, core::ParamValues& values, ParamEvent& event) {
        DrainResult result;
        queue.drainCoalesced(values, result, &event, 1);
        return result;
    }
    static void setProducerSequence(ParamTransactionQueue& queue, std::size_t index, std::uint64_t sequence) {
        queue.producerSequence_[index] = sequence;
    }
};
}  // namespace cd::state

namespace {
constexpr std::size_t kMix = core::indexOf("mix");
constexpr std::size_t kKeyMask = core::indexOf("key.mask");
constexpr std::size_t kDrive = core::indexOf("slot.a.stage.drive");
constexpr std::size_t kHpf = core::indexOf("hpf.frequency");

void testBasics() {
    ParamTransactionQueue q;
    core::ParamValues v = core::defaultValues();
    CHECK(q.push(kMix, 0.25, 7, 3, GestureKind::Begin));
    CHECK(q.push(kDrive, 99.0));  // clamped to 36
    CHECK(!q.push(kDrive, std::nan("")));
    CHECK(!q.push(std::uint32_t(core::kParamCount), 1.0));
    ParamEvent rec[4];
    const DrainResult r = q.drain(v, rec, 4);
    CHECK(r.applied == 2 && r.recorded == 2 && r.coalesced == 0);
    CHECK(v[kMix] == 0.25 && v[kDrive] == 36.0);
    CHECK(rec[0].index == kMix && rec[0].sampleOffset == 7 && rec[0].gestureId == 3 && rec[0].gesture == GestureKind::Begin);
    const auto d = q.diagnostics();
    CHECK(d.pushed == 2 && d.rejected == 2 && d.drained == 2 && d.overflowed == 0 && d.ownershipViolations == 0);
    CHECK(q.drain(v).applied == 0);
}

void testOverflowCoalescing() {
    ParamTransactionQueue q;
    core::ParamValues v = core::defaultValues();
    // Fill the ring completely with one parameter, then keep pushing: values must coalesce.
    for (std::size_t i = 0; i < ParamTransactionQueue::kCapacity; ++i) CHECK(q.push(kHpf, 20.0 + double(i % 100)));
    CHECK(q.diagnostics().overflowed == 0);
    for (int i = 0; i < 500; ++i) CHECK(q.push(kMix, double(i) / 1000.0));  // ring full -> coalesced
    CHECK(q.push(kHpf, 111.0));                                              // also coalesced
    CHECK(q.push(kHpf, 123.0));
    const auto before = q.diagnostics();
    CHECK(before.overflowed == 502);
    const DrainResult r = q.drain(v);
    CHECK(r.coalesced == 2);  // one slot per overflowed parameter, latest value only
    CHECK(v[kMix] == 499.0 / 1000.0);
    CHECK(v[kHpf] == 123.0);  // coalesced (newest) applied after older ring entries
    // After a drain the ring accepts again; ordering for the same parameter stays correct.
    CHECK(q.push(kHpf, 50.0));
    q.drain(v);
    CHECK(v[kHpf] == 50.0);
    CHECK(q.diagnostics().ownershipViolations == 0);
}

void testCoalescedValueOvertakesRingSnapshot() {
    ParamTransactionQueue q;
    core::ParamValues values = core::defaultValues();
    values[kMix] = 0.0;
    for (std::size_t i = 0; i < ParamTransactionQueue::kCapacity; ++i) CHECK(q.push(kHpf, 20.0));

    // The consumer snapshots and releases the old ring, then is descheduled
    // before reading coalesced values. The producer fills the newly available
    // ring and publishes a newer value for the same parameter in its mailbox.
    CHECK(ParamTransactionQueueTestAccess::ring(q, values).applied == ParamTransactionQueue::kCapacity);
    CHECK(q.push(kMix, 0.25, 13, 25, GestureKind::Update));
    for (std::size_t i = 1; i < ParamTransactionQueue::kCapacity; ++i) CHECK(q.push(kHpf, 30.0));
    CHECK(q.push(kMix, 0.75, 23, 75, GestureKind::End));

    ParamEvent latest;
    const auto coalesced = ParamTransactionQueueTestAccess::coalesced(q, values, latest);
    CHECK(coalesced.applied == 1 && coalesced.coalesced == 1 && coalesced.recorded == 1);
    CHECK(values[kMix] == 0.75 && latest.value == 0.75 && latest.gestureId == 75);
    CHECK(latest.sampleOffset == 0 && latest.gesture == GestureKind::None);

    // The next drain must discard the older ring value, including its record.
    std::array<ParamEvent, ParamTransactionQueue::kCapacity> records{};
    const auto next = q.drain(values, records.data(), records.size());
    CHECK(values[kMix] == 0.75 && next.applied == ParamTransactionQueue::kCapacity - 1);
    for (std::size_t i = 0; i < next.recorded; ++i) CHECK(records[i].index != kMix);
    CHECK(q.drain(values).applied == 0);

    // Sequence order, not numerical value, determines freshness: a legitimate
    // later reduction still reaches both the value snapshot and event record.
    CHECK(q.push(kMix, 0.125, 9, 125, GestureKind::Update));
    CHECK(q.drain(values, records.data(), records.size()).recorded == 1);
    CHECK(values[kMix] == 0.125 && records[0].value == 0.125 && records[0].gestureId == 125);
}

void testCoalescedRecordIntegrity() {
    ParamTransactionQueue q;
    for (std::size_t i = 0; i < ParamTransactionQueue::kCapacity; ++i) CHECK(q.push(kHpf, 20.0));
    constexpr std::uint32_t kLastValue = 4000;
    std::latch firstOverflow{1}, consumerStarted{1};
    std::atomic<bool> producerDone{false}, accepted{true};
    std::thread producer([&] {
        if (!q.push(kKeyMask, 1.0, 7, 1, GestureKind::Update)) accepted.store(false);
        firstOverflow.count_down();
        consumerStarted.wait();
        for (std::uint32_t n = 2; n <= kLastValue; ++n) {
            if (!q.push(kKeyMask, double(n), n % 32, n, GestureKind::Update)) accepted.store(false);
            if (n % 31 == 0) std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });
    core::ParamValues values = core::defaultValues();
    std::array<ParamEvent, ParamTransactionQueue::kCapacity + core::kParamCount> records{};
    bool monotonic = true, coherent = true, bounded = true;
    double last = 0;
    firstOverflow.wait();
    consumerStarted.count_down();
    for (;;) {
        const bool done = producerDone.load(std::memory_order_acquire);
        const auto result = q.drain(values, records.data(), records.size());
        if (result.applied > records.size() || result.droppedFromRecord != 0) bounded = false;
        for (std::size_t i = 0; i < result.recorded; ++i) {
            const auto& event = records[i];
            if (event.index != kKeyMask) continue;
            if (event.value <= last) monotonic = false;
            if (event.value != double(event.gestureId)) coherent = false;
            last = event.value;
        }
        if (done) break;
        std::this_thread::yield();
    }
    producer.join();
    CHECK(accepted.load() && monotonic && coherent && bounded);
    CHECK(values[kKeyMask] == double(kLastValue) && last == double(kLastValue));
    CHECK(q.diagnostics().overflowed > 0 && q.diagnostics().ownershipViolations == 0);
}

void testSequenceExhaustion() {
    ParamTransactionQueue q;
    ParamTransactionQueueTestAccess::setProducerSequence(q, kMix, std::numeric_limits<std::uint64_t>::max() - 1);
    CHECK(q.push(kMix, 0.5));
    CHECK(!q.push(kMix, 0.75));
    core::ParamValues values = core::defaultValues();
    CHECK(q.drain(values).applied == 1 && values[kMix] == 0.5);
    CHECK(q.diagnostics().rejected == 1);
}

void testStressSpsc() {
    ParamTransactionQueue q;
    constexpr std::uint32_t kPerParam = 4000;  // values 1..4000 are in key.mask range and strictly increasing
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        for (std::uint32_t n = 1; n <= kPerParam; ++n) {
            q.push(kKeyMask, double(n));
            q.push(kMix, double(n) / double(kPerParam));
            q.push(kDrive, 36.0 * double(n) / double(kPerParam));
            q.push(kHpf, 10.0 + 240.0 * double(n) / double(kPerParam));
            if (n % 500 == 0) std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });
    core::ParamValues v = core::defaultValues();
    v[kKeyMask] = v[kMix] = v[kDrive] = v[kHpf] = 0.0;  // below every pushed value
    double last[4] = {0, 0, 0, 0};
    bool monotonic = true;
    long loops = 0;
    for (;;) {
        const bool done = producerDone.load(std::memory_order_acquire);
        q.drain(v);
        const double now[4] = {v[kKeyMask], v[kMix], v[kDrive], v[kHpf]};
        for (int k = 0; k < 4; ++k) {
            if (now[k] < last[k]) monotonic = false;  // never a stale overwrite
            if (now[k] > last[k]) last[k] = now[k];
        }
        if (done) break;
        if (++loops % 20 == 0) std::this_thread::sleep_for(std::chrono::microseconds(300));  // force overflow
    }
    producer.join();
    q.drain(v);  // anything published after the last loop iteration
    CHECK(monotonic);
    CHECK(v[kKeyMask] == double(kPerParam));
    CHECK(v[kMix] == 1.0 && v[kDrive] == 36.0 && v[kHpf] == 250.0);  // no lost final values
    const auto d = q.diagnostics();
    CHECK(d.ownershipViolations == 0);
    CHECK(d.pushed + d.overflowed + d.rejected == 4ull * kPerParam);
    std::printf("  stress: pushed=%llu coalesced=%llu\n", (unsigned long long)d.pushed, (unsigned long long)d.overflowed);
}

void testNoAllocation() {
    auto q = std::make_unique<ParamTransactionQueue>();
    auto pub = std::make_unique<ParamSnapshotPublisher>();
    core::ParamValues v = core::defaultValues();
    ParamEvent rec[16];
    g_allocs.store(0);
    g_count.store(true);
    for (int i = 0; i < 3000; ++i) q->push(kMix, double(i % 100) / 100.0);  // includes overflow path
    const DrainResult r = q->drain(v, rec, 16);
    pub->publish(v);
    core::ParamValues out{};
    const std::uint64_t gen = pub->read(out);
    g_count.store(false);
    CHECK(g_allocs.load() == 0);
    CHECK(r.applied > 0 && gen == 1 && out == v);
    static_assert(noexcept(q->drain(v)) && noexcept(pub->publish(v)) && noexcept(pub->read(out)) && noexcept(q->push(ParamEvent{})));
}

void testSnapshotPublisher() {
    ParamSnapshotPublisher pub;
    core::ParamValues out{};
    out.fill(-17.0);
    const core::ParamValues original = out;
    CHECK(pub.read(out) == 0);  // nothing published yet
    CHECK(out == original);
    std::atomic<bool> stop{false};
    std::latch firstPublished{1}, firstRead{1};
    std::thread writer([&] {
        core::ParamValues v{};
        v.fill(1.0);
        pub.publish(v);
        firstPublished.count_down();
        firstRead.wait();
        for (std::uint64_t n = 2; n <= 200000; ++n) {
            v.fill(double(n));
            pub.publish(v);
        }
        stop.store(true);
    });
    // Guarantee a real published read before starting the racing stress phase.
    // A wait-free reader is allowed to return 0 while the writer is busy; Windows
    // can schedule the entire writer loop before any successful racing read.
    firstPublished.wait();
    std::uint64_t lastGen = pub.read(out);
    CHECK(lastGen == 1);
    for (double x : out) CHECK(x == 1.0);
    long reads = lastGen == 1 ? 1 : 0;
    firstRead.count_down();
    bool coherent = true, monotonic = true, failurePreservesPrevious = true;
    while (!stop.load(std::memory_order_relaxed)) {
        const core::ParamValues previous = out;
        const std::uint64_t g = pub.read(out, 1);
        if (g == 0) {
            if (out != previous) failurePreservesPrevious = false;
            continue;
        }
        ++reads;
        for (double x : out) if (x != out[0]) coherent = false;  // never a torn mix of two snapshots
        if (out[0] != double(g)) coherent = false;               // value matches its generation tag
        if (g < lastGen) monotonic = false;
        lastGen = g;
    }
    writer.join();
    CHECK(coherent);
    CHECK(monotonic);
    CHECK(failurePreservesPrevious);
    CHECK(reads > 0);
    CHECK(pub.read(out) == 200000 && out[0] == 200000.0);
    const core::ParamValues latest = out;
    CHECK(pub.read(out, 0) == 0 && out == latest);
}
}  // namespace

int main() {
    testBasics();
    testOverflowCoalescing();
    testCoalescedValueOvertakesRingSnapshot();
    testCoalescedRecordIntegrity();
    testSequenceExhaustion();
    testStressSpsc();
    testNoAllocation();
    testSnapshotPublisher();
    return TEST_RESULT("test_queue");
}
