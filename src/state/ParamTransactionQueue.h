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
// per-parameter coalescing slot (latest wins) and QueueDiagnostics::overflowed is incremented. A
// parameter that has an unconsumed coalesced value keeps being routed to its coalescing slot, so
// ordering for one parameter is preserved (older ring entries are applied first, then the latest
// coalesced value). A coalesced value is applied at sample offset 0 and carries no gesture kind; the
// final value of every parameter is never lost.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    std::uint64_t rejected = 0;             // bad index or non-finite value
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

    ParamTransactionQueue() noexcept {
        for (auto& f : pendingFlag_) f.store(0, std::memory_order_relaxed);
        for (auto& v : pendingValue_) v.store(0.0, std::memory_order_relaxed);
        for (auto& g : pendingGesture_) g.store(0, std::memory_order_relaxed);
    }
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
        // A parameter with an unconsumed coalesced value stays on the coalescing path (ordering).
        if (pendingFlag_[e.index].load(std::memory_order_acquire) != 0) { coalesce(e); return true; }
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        if (t - h >= kCapacity) { coalesce(e); return true; }
        ring_[t & (kCapacity - 1)] = e;
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
    // values. If `record` is non-null, the first `recordCap` applied events are copied there (for
    // sample-accurate segmentation by the runtime). Bounded work, noexcept, no allocation.
    DrainResult drain(core::ParamValues& values, ParamEvent* record = nullptr, std::size_t recordCap = 0) noexcept {
        DrainResult r;
        const Guard guard(consumerBusy_, violations_);
        if (!guard.ok) return r;
        const std::uint64_t t = tail_.load(std::memory_order_acquire);  // snapshot bounds the loop
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        for (; h != t; ++h) {
            const ParamEvent& e = ring_[h & (kCapacity - 1)];
            values[e.index] = e.value;
            note(r, record, recordCap, e);
        }
        head_.store(h, std::memory_order_release);
        for (std::uint32_t i = 0; i < core::kParamCount; ++i) {
            if (pendingFlag_[i].load(std::memory_order_relaxed) == 0) continue;
            // exchange (acq_rel) pairs with the producer's release store of the flag, so the value
            // read below is at least as new as the one that set the flag.
            if (pendingFlag_[i].exchange(0, std::memory_order_acq_rel) == 0) continue;
            ParamEvent e;
            e.index = i;
            e.value = pendingValue_[i].load(std::memory_order_relaxed);
            e.gestureId = pendingGesture_[i].load(std::memory_order_relaxed);
            values[i] = e.value;
            ++r.coalesced;
            note(r, record, recordCap, e);
        }
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
    void coalesce(const ParamEvent& e) noexcept {
        pendingValue_[e.index].store(e.value, std::memory_order_relaxed);
        pendingGesture_[e.index].store(e.gestureId, std::memory_order_relaxed);
        pendingFlag_[e.index].store(1, std::memory_order_release);
        overflowed_.fetch_add(1, std::memory_order_relaxed);
    }
    static void note(DrainResult& r, ParamEvent* rec, std::size_t cap, const ParamEvent& e) noexcept {
        ++r.applied;
        if (rec && r.recorded < cap) rec[r.recorded++] = e; else ++r.droppedFromRecord;
    }

    static_assert(std::atomic<double>::is_always_lock_free, "coalescing slots need lock-free atomic<double>");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "ring indices need lock-free 64-bit atomics");

    alignas(64) std::atomic<std::uint64_t> tail_{0};  // written by the producer
    alignas(64) std::atomic<std::uint64_t> head_{0};  // written by the consumer
    alignas(64) std::array<ParamEvent, kCapacity> ring_{};
    std::array<std::atomic<double>, core::kParamCount> pendingValue_{};
    std::array<std::atomic<std::uint32_t>, core::kParamCount> pendingGesture_{};
    std::array<std::atomic<std::uint8_t>, core::kParamCount> pendingFlag_{};
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
    // or no coherent copy was obtained within maxAttempts (the caller keeps its previous snapshot).
    std::uint64_t read(core::ParamValues& out, int maxAttempts = 32) const noexcept {
        for (int a = 0; a < maxAttempts; ++a) {
            const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == 0) return 0;
            if (s1 & 1u) continue;
            for (std::size_t i = 0; i < core::kParamCount; ++i) out[i] = data_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) return s1 / 2;
        }
        return 0;
    }
    std::uint64_t generation() const noexcept { return seq_.load(std::memory_order_acquire) / 2; }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "seqlock counter must be lock-free");
    alignas(64) std::atomic<std::uint64_t> seq_{0};
    std::array<std::atomic<double>, core::kParamCount> data_{};
};

}  // namespace cd::state
