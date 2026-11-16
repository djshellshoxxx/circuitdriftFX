// CircuitDrift state: bounded wait-free single-producer/single-consumer ring (P04, UX-09).
// Owner: cd_state. Original implementation (no third-party code); the design is the
// well-known Lamport ring with cached opposite indices.
// Ownership/threading: exactly ONE producer thread calls tryPush and exactly ONE consumer
// thread calls tryPop/peek (never use as MPMC). Payload storage is allocated once in the
// constructor (non-audio); tryPush/tryPop never allocate, lock, block or throw.
// Indices are monotonically increasing 64-bit counters (no wrap in practice); the producer
// publishes with release, the consumer observes with acquire, and vice versa for the free
// space. Producer and consumer indices live on separate cache lines.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace cd::state {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue payload must be trivially copyable");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");

public:
    // Capacity is rounded up to a power of two; minCapacity must be in [1, 2^30].
    explicit SpscQueue(std::size_t minCapacity) {
        if (minCapacity == 0 || minCapacity > (std::size_t(1) << 30))
            throw std::invalid_argument("SpscQueue capacity out of range");
        std::size_t c = 1;
        while (c < minCapacity) c <<= 1;
        capacity_ = c;
        mask_ = c - 1;
        slots_ = std::make_unique<T[]>(c);
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    // Producer only. Returns false (and changes nothing) when fewer than keepFree+1 slots are
    // free; keepFree lets a producer reserve the last slots for a higher-priority class.
    bool tryPush(const T& v, std::size_t keepFree = 0) noexcept {
        const std::uint64_t tail = prod_.tail.load(std::memory_order_relaxed);
        if (keepFree >= capacity_) return false;
        const std::uint64_t limit = capacity_ - keepFree;
        if (tail - prod_.headCache >= limit) {
            prod_.headCache = cons_.head.load(std::memory_order_acquire);
            if (tail - prod_.headCache >= limit) return false;
        }
        slots_[tail & mask_] = v;
        prod_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns false when empty.
    bool tryPop(T& out) noexcept {
        const std::uint64_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.tailCache) {
            cons_.tailCache = prod_.tail.load(std::memory_order_acquire);
            if (head == cons_.tailCache) return false;
        }
        out = slots_[head & mask_];
        cons_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate occupancy; exact only when called from a quiescent state.
    std::size_t sizeApprox() const noexcept {
        const std::uint64_t t = prod_.tail.load(std::memory_order_acquire);
        const std::uint64_t h = cons_.head.load(std::memory_order_acquire);
        return t >= h ? std::size_t(t - h) : 0;
    }

private:
    struct alignas(kCacheLine) Producer {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t headCache = 0;  // producer-private copy of consumer head
    };
    struct alignas(kCacheLine) Consumer {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t tailCache = 0;  // consumer-private copy of producer tail
    };
    Producer prod_;
    Consumer cons_;
    alignas(kCacheLine) std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::unique_ptr<T[]> slots_;
};

}  // namespace cd::state
