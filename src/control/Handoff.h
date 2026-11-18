// CircuitDrift control: prepared-object handoff with generation check and off-audio reclamation.
// Spec 01: "Handle reclamation occurs on a worker after the audio consumer acknowledges its
// generation; no reference-count destruction/deallocation on audio."
// Ownership/roles:
//   Worker (any non-audio thread, serialised internally): builds a T, install()s it, reclaim()s.
//   Audio (single thread): take() swaps a pending node in at a block boundary; current() reads it.
// The audio thread never constructs or destroys a T or a Node. A node it stops using (replaced, or
// rejected for an old generation) is pushed onto a bounded retire ring; the push IS the
// acknowledgement that audio no longer references it, and only then may the worker delete it.
// Pressure policy: at most one pending install (a newer install replaces an unconsumed older one,
// which is returned to the worker for deletion). If the retire ring is full, take() defers
// (returns Deferred, leaves the pending node untouched); reclaim() on the worker frees space.
// Limit: destructor must run after the audio thread has stopped using the slot.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include "control/SpscRing.h"

namespace cd::control {

template <class T>
class HandoffSlot {
    static_assert(std::atomic<void*>::is_always_lock_free, "pointer atomics must be lock-free");

public:
    struct Node {
        std::unique_ptr<T> payload;
        std::uint64_t generation = 0;
    };
    static constexpr std::size_t kRetireCapacity = 8;
    enum class Take : unsigned char { None, Installed, RejectedStale, Deferred };

    HandoffSlot() = default;
    HandoffSlot(const HandoffSlot&) = delete;
    HandoffSlot& operator=(const HandoffSlot&) = delete;
    ~HandoffSlot() {
        delete pending_.exchange(nullptr);
        delete current_;
        Node* n = nullptr;
        while (retire_.tryPop(n)) delete n;
    }

    // ---- Worker role ----
    // Allocates (worker only). Returns an older unconsumed pending node, if any, to delete.
    std::unique_ptr<Node> install(std::unique_ptr<T> payload, std::uint64_t generation) {
        auto* n = new Node{std::move(payload), generation};
        std::lock_guard<std::mutex> lk(workerMutex_);
        return std::unique_ptr<Node>(pending_.exchange(n, std::memory_order_acq_rel));
    }
    // Deletes every node audio has retired. Returns the number reclaimed.
    std::size_t reclaim() {
        std::lock_guard<std::mutex> lk(workerMutex_);
        std::size_t n = 0;
        Node* p = nullptr;
        while (retire_.tryPop(p)) {
            delete p;
            ++n;
        }
        return n;
    }

    // ---- Audio role (block boundary) ----
    Take take(std::uint64_t audioGeneration) noexcept {
        if (pending_.load(std::memory_order_relaxed) == nullptr) return Take::None;
        if (retire_.freeSlots() < 1) return Take::Deferred;  // free slot is reserved for this push
        Node* n = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (n == nullptr) return Take::None;
        if (n->generation != audioGeneration) {
            retire_.tryPush(n);
            return Take::RejectedStale;
        }
        Node* old = current_;
        current_ = n;
        if (old != nullptr) retire_.tryPush(old);
        return Take::Installed;
    }
    // Audio: currently installed payload or nullptr.
    T* current() const noexcept { return current_ != nullptr ? current_->payload.get() : nullptr; }
    std::uint64_t currentGeneration() const noexcept { return current_ != nullptr ? current_->generation : 0; }

private:
    std::atomic<Node*> pending_{nullptr};
    Node* current_ = nullptr;                  // audio-owned
    SpscRing<Node*, kRetireCapacity> retire_;  // producer: audio, consumer: worker
    std::mutex workerMutex_;                   // serialises worker threads only; audio never touches it
};

}  // namespace cd::control
