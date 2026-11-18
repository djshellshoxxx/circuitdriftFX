// CircuitDrift control: wait-free "latest value" mailbox (triple buffer / generation slot).
// One producer publishes whole coherent objects; one consumer acquires the newest published one.
// Intermediate publications that the consumer did not acquire are overwritten (coalesced):
// this is the documented pressure policy, so the slot can never be full and never blocks.
// Ownership: slot owns three inline T buffers. Producer owns writeBuffer() until publish();
// consumer owns the buffer returned by acquire() until its next acquire(). Single producer and
// single consumer thread only (callers serialise several non-audio producers themselves).
// Threading: all methods noexcept, wait-free (one atomic exchange), no allocation.
// publish() recycles a buffer holding an OLD value: the producer must fully overwrite it.
#pragma once
#include <atomic>

namespace cd::control {

template <class T>
class LatestSlot {
    static_assert(std::atomic<unsigned>::is_always_lock_free, "atomic<unsigned> must be lock-free");

public:
    // Producer.
    T& writeBuffer() noexcept { return buf_[back_]; }
    void publish() noexcept {
        const unsigned old = mid_.exchange(back_ | kDirty, std::memory_order_acq_rel);
        back_ = old & kIndexMask;
    }
    // Consumer. Returns the newest unseen publication or nullptr when nothing new.
    // The pointer stays valid (and unmodified) until the next acquire() call.
    const T* acquire() noexcept {
        if (!(mid_.load(std::memory_order_relaxed) & kDirty)) return nullptr;
        const unsigned old = mid_.exchange(front_, std::memory_order_acq_rel);
        front_ = old & kIndexMask;
        return &buf_[front_];
    }
    // Consumer: buffer returned by the last successful acquire() (value-initialised before one).
    const T& current() const noexcept { return buf_[front_]; }

private:
    static constexpr unsigned kDirty = 4u, kIndexMask = 3u;
    alignas(64) T buf_[3]{};
    alignas(64) std::atomic<unsigned> mid_{1u};  // index of the middle buffer | dirty flag
    alignas(64) unsigned back_ = 2u;             // producer-private
    alignas(64) unsigned front_ = 0u;            // consumer-private
};

}  // namespace cd::control
