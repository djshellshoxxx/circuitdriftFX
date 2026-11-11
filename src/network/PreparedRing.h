// CircuitDrift network: prepared SPSC frame rings in a (shared) region (P15, CD-F11/T11).
// Owner: cd_network.
// One ring per member and direction (member->broker Demand, broker->member Result), fixed
// kRingQuanta (64) slots of one whole frame (header + 128-frame mono/stereo payload) each.
// Memory comes from a RingAllocator/SharedRegion interface, allocated OFF audio before use.
// HeapRingAllocator is the in-process mock (heap memory). Real OS shared memory (per-user object
// permissions, protocol versioned names, sandbox handling) is a platform adapter: gate G05, open.
//
// Protocol: the producer obtains the next free slot (beginWrite), writes the complete frame in
// place and publishes it with ONE release store of the tail (commit). The consumer performs ONE
// acquire load of the tail per acquire() and then reads only the slots [head, head+n) it owns,
// returning them with a release store of the head. A slot is never read while it can be
// written. There is no spin-until-ready loop and no seqlock retry anywhere: a full ring makes
// beginWrite() return nullptr (frame dropped and counted), an empty ring returns 0.
// All producer/consumer operations are noexcept, bounded and allocation/lock/syscall free.
// Cross-process note: std::atomic<uint64_t> is static_asserted always-lock-free here, which is
// necessary but NOT sufficient for an IPC primitive; the address-free atomic layout/ABI across
// processes (and across 32/64-bit or differently compiled peers) must be validated per platform
// (G05). The ring control block carries magic/version/slot geometry for that adapter check.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include "network/Protocol.h"

namespace cd::network {

inline constexpr std::size_t kRingAlign = 64;

class SharedRegion {
public:
    virtual ~SharedRegion() = default;
    virtual std::byte* data() noexcept = 0;
    virtual std::size_t size() const noexcept = 0;
};

enum class RingDirection : std::uint32_t { MemberToBroker = 1, BrokerToMember = 2, Control = 3 };

struct RegionRequest {
    std::size_t bytes = 0;
    std::size_t alignment = kRingAlign;
    Uuid128 group;
    Uuid128 member;
    RingDirection direction = RingDirection::Control;
};

// Non-audio. May return nullptr (allocation/mapping failure).
class RingAllocator {
public:
    virtual ~RingAllocator() = default;
    virtual std::unique_ptr<SharedRegion> allocate(const RegionRequest& request) = 0;
};

// In-process mock: zero-filled aligned heap memory. Counts allocations for tests.
class HeapRingAllocator final : public RingAllocator {
public:
    std::unique_ptr<SharedRegion> allocate(const RegionRequest& request) override;
    std::size_t allocations() const noexcept { return allocations_; }
    void failNext(bool f) noexcept { failNext_ = f; }
private:
    std::size_t allocations_ = 0;
    bool failNext_ = false;
};

struct alignas(kRingAlign) RingIndex {
    std::atomic<std::uint64_t> value{0};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "ring indices must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "control words must be lock-free");

struct RingControl {
    RingIndex head;    // written by consumer only
    RingIndex tail;    // written by producer only
    std::uint32_t magic = 0x52504443u;  // 'CDPR'
    std::uint32_t version = 1;
    std::uint32_t slotBytes = 0;
    std::uint32_t slotCount = 0;
};

template <class Frame>
class PreparedRing {
    static_assert(std::is_trivially_copyable_v<Frame> && std::is_standard_layout_v<Frame>,
                  "ring payload must be POD (no pointers by protocol)");

public:
    static constexpr std::size_t kSlots = kRingQuanta;
    static_assert((kSlots & (kSlots - 1)) == 0);
    static constexpr std::size_t kSlotsOffset = (sizeof(RingControl) + kRingAlign - 1) / kRingAlign * kRingAlign;
    static constexpr std::size_t kSlotStride = (sizeof(Frame) + kRingAlign - 1) / kRingAlign * kRingAlign;
    static constexpr std::size_t regionBytes() noexcept { return kSlotsOffset + kSlots * kSlotStride; }

    // Non-audio: constructs the control block and value-initialises every slot in the region.
    explicit PreparedRing(std::unique_ptr<SharedRegion> region) : region_(std::move(region)) {
        if (!region_ || region_->size() < regionBytes() ||
            reinterpret_cast<std::uintptr_t>(region_->data()) % kRingAlign != 0)
            throw std::invalid_argument("PreparedRing: region too small or misaligned");
        std::byte* base = region_->data();
        control_ = ::new (static_cast<void*>(base)) RingControl{};
        control_->slotBytes = static_cast<std::uint32_t>(sizeof(Frame));
        control_->slotCount = static_cast<std::uint32_t>(kSlots);
        slots_ = base + kSlotsOffset;
        for (std::size_t i = 0; i < kSlots; ++i) ::new (static_cast<void*>(slots_ + i * kSlotStride)) Frame{};
    }
    PreparedRing(const PreparedRing&) = delete;
    PreparedRing& operator=(const PreparedRing&) = delete;
    ~PreparedRing() = default;   // RingControl/Frame are trivially destructible

    // ---- producer ----
    // Next free slot to fill completely, or nullptr when the ring is full (drop counted).
    Frame* beginWrite() noexcept {
        const std::uint64_t tail = control_->tail.value.load(std::memory_order_relaxed);
        const std::uint64_t head = control_->head.value.load(std::memory_order_acquire);
        if (tail - head >= kSlots) { ++producerDrops_; return nullptr; }
        writing_ = true;
        return slot(tail);
    }
    // Publishes the slot returned by the last beginWrite() (one release store).
    void commit() noexcept {
        if (!writing_) return;
        writing_ = false;
        const std::uint64_t tail = control_->tail.value.load(std::memory_order_relaxed);
        control_->tail.value.store(tail + 1, std::memory_order_release);
    }
    std::uint64_t producerDrops() const noexcept { return producerDrops_; }

    // ---- consumer ----
    // One acquire of the producer tail; returns how many published slots the consumer now owns.
    std::size_t acquire() noexcept {
        acquiredHead_ = control_->head.value.load(std::memory_order_relaxed);
        const std::uint64_t tail = control_->tail.value.load(std::memory_order_acquire);
        acquired_ = static_cast<std::size_t>(tail - acquiredHead_);
        return acquired_;
    }
    // i < value returned by the last acquire().
    const Frame& at(std::size_t i) const noexcept { return *slot(acquiredHead_ + i); }
    // Returns n (<= acquired) slots to the producer (one release store).
    void release(std::size_t n) noexcept {
        if (n > acquired_) n = acquired_;
        control_->head.value.store(acquiredHead_ + n, std::memory_order_release);
        acquiredHead_ += n;
        acquired_ -= n;
    }

    // Approximate occupancy (diagnostics only).
    std::size_t sizeApprox() const noexcept {
        const std::uint64_t t = control_->tail.value.load(std::memory_order_acquire);
        const std::uint64_t h = control_->head.value.load(std::memory_order_acquire);
        return t >= h ? static_cast<std::size_t>(t - h) : 0;
    }
    const RingControl& control() const noexcept { return *control_; }

private:
    Frame* slot(std::uint64_t index) const noexcept {
        return std::launder(reinterpret_cast<Frame*>(slots_ + (index & (kSlots - 1)) * kSlotStride));
    }
    std::unique_ptr<SharedRegion> region_;
    RingControl* control_ = nullptr;
    std::byte* slots_ = nullptr;
    // producer-private
    bool writing_ = false;
    std::uint64_t producerDrops_ = 0;
    // consumer-private
    std::uint64_t acquiredHead_ = 0;
    std::size_t acquired_ = 0;
};

// Per-member control words shared between the member (audio) and the broker (worker).
enum class BrokerState : std::uint32_t { Unknown = 0, Alive = 1, Gone = 2 };
enum class MembershipState : std::uint32_t { Unknown = 0, Active = 1, Removed = 2 };

struct ChannelControl {
    alignas(kRingAlign) std::atomic<std::uint64_t> memberHeartbeat{0};   // member increments per callback
    alignas(kRingAlign) std::atomic<std::uint32_t> brokerState{0};       // BrokerState
    std::atomic<std::uint32_t> membership{0};                            // MembershipState
};

// Identity the broker assigns at join; read by the member off-audio in connect().
struct ChannelIdentity {
    Uuid128 group;
    Uuid128 session;
    Uuid128 member;
    std::uint64_t joinGeneration = 0;
    double sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t railModelId = 0;
};

// Both rings plus control words of one member. Created by the broker at join (non-audio).
class MemberChannel {
public:
    MemberChannel(RingAllocator& alloc, const ChannelIdentity& id);
    MemberChannel(const MemberChannel&) = delete;
    MemberChannel& operator=(const MemberChannel&) = delete;

    const ChannelIdentity& identity() const noexcept { return id_; }
    ChannelControl& control() noexcept { return *control_; }
    PreparedRing<DemandFrame>& uplink() noexcept { return *up_; }
    PreparedRing<ResultFrame>& downlink() noexcept { return *down_; }

private:
    static std::unique_ptr<SharedRegion> need(RingAllocator& a, const RegionRequest& r);
    ChannelIdentity id_;
    std::unique_ptr<SharedRegion> controlRegion_;
    ChannelControl* control_ = nullptr;
    std::unique_ptr<PreparedRing<DemandFrame>> up_;
    std::unique_ptr<PreparedRing<ResultFrame>> down_;
};

}  // namespace cd::network
