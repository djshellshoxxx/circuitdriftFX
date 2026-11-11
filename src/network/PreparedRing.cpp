// CircuitDrift network: heap mock region and member channel construction (non-audio).
#include "network/PreparedRing.h"
#include <cstring>

namespace cd::network {

namespace {
class HeapRegion final : public SharedRegion {
public:
    HeapRegion(std::size_t bytes, std::size_t align) : bytes_(bytes), align_(align) {
        data_ = static_cast<std::byte*>(::operator new(bytes, std::align_val_t(align)));
        std::memset(data_, 0, bytes);
    }
    ~HeapRegion() override { ::operator delete(data_, std::align_val_t(align_)); }
    std::byte* data() noexcept override { return data_; }
    std::size_t size() const noexcept override { return bytes_; }
private:
    std::byte* data_ = nullptr;
    std::size_t bytes_ = 0;
    std::size_t align_ = 0;
};
}  // namespace

std::unique_ptr<SharedRegion> HeapRingAllocator::allocate(const RegionRequest& r) {
    if (failNext_) { failNext_ = false; return nullptr; }
    if (r.bytes == 0 || r.alignment == 0 || (r.alignment & (r.alignment - 1)) != 0) return nullptr;
    ++allocations_;
    return std::make_unique<HeapRegion>(r.bytes, r.alignment < kRingAlign ? kRingAlign : r.alignment);
}

std::unique_ptr<SharedRegion> MemberChannel::need(RingAllocator& a, const RegionRequest& r) {
    auto region = a.allocate(r);
    if (!region) throw std::runtime_error("MemberChannel: region allocation failed");
    return region;
}

MemberChannel::MemberChannel(RingAllocator& alloc, const ChannelIdentity& id) : id_(id) {
    RegionRequest r;
    r.group = id.group;
    r.member = id.member;
    r.direction = RingDirection::Control;
    r.bytes = sizeof(ChannelControl);
    controlRegion_ = need(alloc, r);
    if (controlRegion_->size() < sizeof(ChannelControl) ||
        reinterpret_cast<std::uintptr_t>(controlRegion_->data()) % alignof(ChannelControl) != 0)
        throw std::runtime_error("MemberChannel: control region too small or misaligned");
    control_ = ::new (static_cast<void*>(controlRegion_->data())) ChannelControl{};
    control_->brokerState.store(static_cast<std::uint32_t>(BrokerState::Alive), std::memory_order_release);
    control_->membership.store(static_cast<std::uint32_t>(MembershipState::Active), std::memory_order_release);
    r.direction = RingDirection::MemberToBroker;
    r.bytes = PreparedRing<DemandFrame>::regionBytes();
    up_ = std::make_unique<PreparedRing<DemandFrame>>(need(alloc, r));
    r.direction = RingDirection::BrokerToMember;
    r.bytes = PreparedRing<ResultFrame>::regionBytes();
    down_ = std::make_unique<PreparedRing<ResultFrame>>(need(alloc, r));
}

}  // namespace cd::network
