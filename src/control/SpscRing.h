// CircuitDrift control: bounded wait-free single-producer/single-consumer ring (P04, UX-09).
// Ownership: storage is inline (no heap, no resize). Exactly ONE thread may call the producer
// methods and exactly ONE thread the consumer methods; these may be different threads. Violating
// that (e.g. two producers) is a data race: callers needing several producers must serialise them
// outside this class. Producer and consumer calls never block, allocate or spin.
// Threading: tryPush/freeSlots = producer; tryPop/size = consumer. All noexcept.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cd::control {

template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "payload must be trivially copyable");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Producer. Returns false (value not stored) when full; never overwrites unread data.
    bool tryPush(const T& v) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        if (t - h >= Capacity) return false;
        buf_[t & (Capacity - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    // Producer. Free slots can only grow while the producer is not pushing, so a positive
    // answer is a guarantee that the next that-many pushes succeed.
    std::size_t freeSlots() const noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(Capacity - (t - h));
    }
    // Consumer. Returns false when empty.
    bool tryPop(T& out) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (h == t) return false;
        out = buf_[h & (Capacity - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    // Consumer: number of readable items (may only grow while the consumer is not popping).
    std::size_t size() const noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(t - h);
    }

private:
    alignas(64) std::atomic<std::uint64_t> head_{0};  // consumer-owned write
    alignas(64) std::atomic<std::uint64_t> tail_{0};  // producer-owned write
    alignas(64) T buf_[Capacity]{};
};

}  // namespace cd::control
