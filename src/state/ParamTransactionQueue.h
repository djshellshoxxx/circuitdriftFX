// CircuitDrift state: parameter transaction queue (message/host thread -> audio thread) and a
// generation-tagged ParamValues snapshot publisher (audio thread -> UI). P04/P07, CD-F09, UX-09.
//
// OWNERSHIP (single producer / single consumer, enforced by cheap in-band checks):
//   ParamTransactionQueue : exactly ONE producer thread calls push() (message or host thread; a
//                           wrapper with several producers must serialise them itself) and exactly
//                           ONE consumer thread (audio) calls drain(). Diagnostics are readable from
//                           any thread. Concurrent misuse is detected and counted in
//                           QueueDiagnostics::ownershipViolations; the violating call is rejected
//                           before it touches the ring.
//   ParamSnapshotPublisher: ONE writer (audio) publish(); one or more readers (UI) read().
//
// AUDIO-THREAD SAFETY: drain() and publish() are noexcept, allocation-free, lock-free (all storage is
// inline in the object; no std::vector/std::function/exceptions) and bounded: drain() performs at
// most kCapacity + kParamCount iterations; publish() copies kParamCount doubles. read() never blocks:
// it retries a bounded number of times and reports failure instead of spinning.
//
// OVERFLOW POLICY: the ring never blocks the producer. When it is full, the value is written into a
// per-parameter coalescing mailbox (latest wins) and QueueDiagnostics::overflowed is incremented.
// The mailbox transfers complete event ownership through three fixed slots; the producer never
// modifies the consumer's slot. Per-parameter sequence numbers suppress older ring entries when a
// newer coalesced value overtakes the consumer's bounded ring snapshot. A parameter with an unread
// coalesced value keeps using its mailbox. Coalesced values use sample offset 0 and no gesture kind;
// the final accepted value of every parameter is never lost. Sequence exhaustion rejects new pushes
// instead of wrapping (after UINT64_MAX accepted events for a single parameter).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/ParameterRegistry.h"

namespace cd::state {

enum class GestureKind : std::uint8_t { None = 0, Begin = 1, Update = 2, End = 3 };

struct ParamEvent {
    std::uint32_t index = 0;         // registry index (kParams[index])
    std::uint32_t sampleOffset = 0;  // offset within the next audio block
    double value = 0.0;              // plain value in registry units
    std::uint32_t gestureId = 0;     // 0 = not part of a gesture
    GestureKind gesture = GestureKind::None;
};

struct QueueDiagnostics {
    std::uint64_t pushed = 0;               // accepted into the ring
    std::uint64_t overflowed = 0;           // pushes routed to the coalescing slots
    std::uint64_t rejected = 0;             // bad index, non-finite value or exhausted sequence
    std::uint64_t drained = 0;              // parameter writes applied by the consumer
    std::uint64_t ownershipViolations = 0;  // concurrent producers/consumers detected
};

struct DrainResult {
    std::size_t applied = 0;            // total parameter writes performed
    std::size_t coalesced = 0;          // of which came from coalescing slots
    std::size_t recorded = 0;           // events written to the caller's record buffer
    std::size_t droppedFromRecord = 0;  // applied events that did not fit the record buffer
};

class ParamTransactionQueue {
public:
    static constexpr std::size_t kCapacity = 1024;  // power of two
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

    ParamTransactionQueue() noexcept = default;
    ParamTransactionQueue(const ParamTransactionQueue&) = delete;
    ParamTransactionQueue& operator=(const ParamTransactionQueue&) = delete;

    // Producer. Never blocks. Out-of-range values are clamped to the registry range (runtime edits,
    // unlike presets, are clamped); non-finite values and bad indices are rejected.
    // Returns true if the event was accepted (ring or coalescing slot).
    bool push(ParamEvent e) noexcept {
        const Guard guard(producerBusy_, violations_);
        if (!guard.ok) return false;
        if (e.index >= core::kParamCount || !std::isfinite(e.value)) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const core::ParamSpec& s = core::kParams[e.index];
        e.value = std::clamp(e.value, s.min, s.max);
        if (producerSequence_[e.index] == std::numeric_limits<std::uint64_t>::max()) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const SequencedEvent queued{e, ++producerSequence_[e.index]};
        // A parameter with an unconsumed coalesced value stays on the coalescing path (ordering).
        if (pending_[e.index].hasPending()) { coalesce(queued); return true; }
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        if (t - h >= kCapacity) { coalesce(queued); return true; }
        ring_[t & (kCapacity - 1)] = queued;
        tail_.store(t + 1, std::memory_order_release);
        pushed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool push(std::uint32_t index, double plainValue, std::uint32_t sampleOffset = 0,
              std::uint32_t gestureId = 0, GestureKind gesture = GestureKind::None) noexcept {
        ParamEvent e;
        e.index = index; e.value = plainValue; e.sampleOffset = sampleOffset;
        e.gestureId = gestureId; e.gesture = gesture;
        return push(e);
    }

    // Consumer (audio thread). Applies queued events in order to `values`, then pending coalesced
    // values. Ring entries already superseded by a newer coalesced value are skipped. If `record`
    // is non-null, the first `recordCap` applied events are copied there (for
    // sample-accurate segmentation by the runtime). Bounded work, noexcept, no allocation.
    DrainResult drain(core::ParamValues& values, ParamEvent* record = nullptr, std::size_t recordCap = 0) noexcept {
        DrainResult r;
        const Guard guard(consumerBusy_, violations_);
        if (!guard.ok) return r;
        drainRing(values, r, record, recordCap);
        drainCoalesced(values, r, record, recordCap);
        drained_.fetch_add(r.applied, std::memory_order_relaxed);
        return r;
    }

    QueueDiagnostics diagnostics() const noexcept {
        QueueDiagnostics d;
        d.pushed = pushed_.load(std::memory_order_relaxed);
        d.overflowed = overflowed_.load(std::memory_order_relaxed);
        d.rejected = rejected_.load(std::memory_order_relaxed);
        d.drained = drained_.load(std::memory_order_relaxed);
        d.ownershipViolations = violations_.load(std::memory_order_relaxed);
        return d;
    }
    // Approximate (racy by design) number of ring entries waiting.
    std::size_t approxPending() const noexcept {
        return std::size_t(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
    }

private:
    // Tests can schedule the two real drain phases deterministically without
    // callbacks, waits or test-only branches in the production audio path.
    friend struct ParamTransactionQueueTestAccess;

    struct SequencedEvent {
        ParamEvent event{};
        std::uint64_t sequence = 0;
    };
    struct CoalescedMailbox {
        static constexpr std::uint8_t kFresh = 4, kIndexMask = 3;
        std::array<SequencedEvent, 3> slots{};
        std::atomic<std::uint8_t> middle{1};
        std::uint8_t back = 0;   // producer-owned
        std::uint8_t front = 2;  // consumer-owned

        bool hasPending() const noexcept { return (middle.load(std::memory_order_acquire) & kFresh) != 0; }
        void publish(const SequencedEvent& event) noexcept {
            slots[back] = event;
            // Release publishes the completed slot; acquire takes ownership of
            // a slot the consumer has finished reading, if it exchanged one.
            back = middle.exchange(std::uint8_t(back | kFresh), std::memory_order_acq_rel) & kIndexMask;
        }
        bool consume(SequencedEvent& event) noexcept {
            if (!hasPending()) return false;
            // One consumer clears the fresh bit. A racing producer can replace
            // middle only with another complete fresh publication.
            front = middle.exchange(front, std::memory_order_acq_rel) & kIndexMask;
            event = slots[front];
            return true;
        }
    };

    struct Guard {
        std::atomic<bool>& busy; bool ok;
        Guard(std::atomic<bool>& b, std::atomic<std::uint64_t>& violations) noexcept
            : busy(b), ok(!b.exchange(true, std::memory_order_acquire)) {
            if (!ok) violations.fetch_add(1, std::memory_order_relaxed);
        }
        ~Guard() { if (ok) busy.store(false, std::memory_order_release); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
    void coalesce(SequencedEvent e) noexcept {
        e.event.sampleOffset = 0;
        e.event.gesture = GestureKind::None;
        pending_[e.event.index].publish(e);
        overflowed_.fetch_add(1, std::memory_order_relaxed);
    }
    bool applyLatest(const SequencedEvent& queued, core::ParamValues& values, DrainResult& result,
                     ParamEvent* record, std::size_t recordCap) noexcept {
        const ParamEvent& e = queued.event;
        if (queued.sequence <= consumedSequence_[e.index]) return false;
        consumedSequence_[e.index] = queued.sequence;
        values[e.index] = e.value;
        note(result, record, recordCap, e);
        return true;
    }
    void drainRing(core::ParamValues& values, DrainResult& result, ParamEvent* record, std::size_t recordCap) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_acquire);  // snapshot bounds the loop
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        for (; h != t; ++h) applyLatest(ring_[h & (kCapacity - 1)], values, result, record, recordCap);
        head_.store(h, std::memory_order_release);
    }
    void drainCoalesced(core::ParamValues& values, DrainResult& result, ParamEvent* record, std::size_t recordCap) noexcept {
        for (std::uint32_t i = 0; i < core::kParamCount; ++i) {
            SequencedEvent e;
            if (pending_[i].consume(e) && applyLatest(e, values, result, record, recordCap)) ++result.coalesced;
        }
    }
    static void note(DrainResult& r, ParamEvent* rec, std::size_t cap, const ParamEvent& e) noexcept {
        ++r.applied;
        if (rec && r.recorded < cap) rec[r.recorded++] = e; else ++r.droppedFromRecord;
    }

    static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "coalescing mailbox needs lock-free slot ownership");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "ring indices need lock-free 64-bit atomics");
    static_assert(std::atomic<bool>::is_always_lock_free, "queue ownership checks need lock-free atomics");

    alignas(64) std::atomic<std::uint64_t> tail_{0};  // written by the producer
    alignas(64) std::atomic<std::uint64_t> head_{0};  // written by the consumer
    alignas(64) std::array<SequencedEvent, kCapacity> ring_{};
    std::array<CoalescedMailbox, core::kParamCount> pending_{};
    std::array<std::uint64_t, core::kParamCount> producerSequence_{};  // producer-owned
    std::array<std::uint64_t, core::kParamCount> consumedSequence_{};  // consumer-owned
    std::atomic<bool> producerBusy_{false}, consumerBusy_{false};
    std::atomic<std::uint64_t> pushed_{0}, overflowed_{0}, rejected_{0}, drained_{0}, violations_{0};
};

// Seqlock-style single-writer snapshot of ParamValues. Every element is an atomic<double> accessed
// with relaxed ordering inside the sequence protocol: no data race and no torn double.
class ParamSnapshotPublisher {
public:
    ParamSnapshotPublisher() noexcept { for (auto& v : data_) v.store(0.0, std::memory_order_relaxed); }
    ParamSnapshotPublisher(const ParamSnapshotPublisher&) = delete;
    ParamSnapshotPublisher& operator=(const ParamSnapshotPublisher&) = delete;

    // Writer (audio thread). Wait-free, bounded.
    void publish(const core::ParamValues& v) noexcept {
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);  // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < core::kParamCount; ++i) data_[i].store(v[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);  // even: complete
    }
    // Reader (UI). Returns the generation (>0) of a coherent copy, or 0 if nothing was published yet
    // or no coherent copy was obtained within maxAttempts. On failure `out` is unchanged.
    std::uint64_t read(core::ParamValues& out, int maxAttempts = 32) const noexcept {
        core::ParamValues candidate{};
        for (int a = 0; a < maxAttempts; ++a) {
            const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == 0) return 0;
            if (s1 & 1u) continue;
            for (std::size_t i = 0; i < core::kParamCount; ++i) candidate[i] = data_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                out = candidate;
                return s1 / 2;
            }
        }
        return 0;
    }
    std::uint64_t generation() const noexcept { return seq_.load(std::memory_order_acquire) / 2; }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "seqlock counter must be lock-free");
    static_assert(std::atomic<double>::is_always_lock_free, "snapshot values must be lock-free");
    alignas(64) std::atomic<std::uint64_t> seq_{0};
    std::array<std::atomic<double>, core::kParamCount> data_{};
};

}  // namespace cd::state
