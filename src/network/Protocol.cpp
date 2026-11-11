// CircuitDrift network: CDIP protocol helpers (see Protocol.h). Owner: cd_network.
// Threading: pure functions; ShmRing push/peek/pop are wait-free and audio-safe.
#include "network/Protocol.h"
#include <cmath>
#include <random>

namespace cd::network {

namespace {
std::uint64_t mix64(std::uint64_t z) noexcept {   // splitmix64 finaliser
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
constexpr std::uint64_t align64(std::uint64_t v) noexcept { return (v + 63u) & ~std::uint64_t(63u); }
}  // namespace

bool Uuid::isNil() const noexcept {
    for (auto v : b) if (v) return false;
    return true;
}

Uuid Uuid::random() {
    std::random_device rd;
    Uuid u;
    for (std::size_t i = 0; i < 16; i += 4) {
        const std::uint32_t r = rd();
        for (std::size_t k = 0; k < 4; ++k) u.b[i + k] = std::uint8_t(r >> (8 * k));
    }
    u.b[6] = std::uint8_t((u.b[6] & 0x0f) | 0x40);   // RFC 4122 v4 marker
    u.b[8] = std::uint8_t((u.b[8] & 0x3f) | 0x80);
    return u;
}

Uuid Uuid::fromSeed(std::uint64_t seed) noexcept {
    Uuid u;
    const std::uint64_t a = mix64(seed), c = mix64(seed ^ 0xa5a5a5a5a5a5a5a5ull);
    for (std::size_t k = 0; k < 8; ++k) { u.b[k] = std::uint8_t(a >> (8 * k)); u.b[8 + k] = std::uint8_t(c >> (8 * k)); }
    return u;
}

bool uuidLess(const Uuid& a, const Uuid& b) noexcept { return a.b < b.b; }

void Hash64::update(const void* p, std::size_t n) noexcept {
    const auto* s = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) { h_ ^= s[i]; h_ *= 0x100000001b3ull; }
}
std::uint64_t Hash64::finish() const noexcept { return mix64(h_); }

std::uint64_t hash64(const void* p, std::size_t n) noexcept {
    Hash64 h;
    h.update(p, n);
    return h.finish();
}

std::uint64_t sessionKeyHash(const Uuid& session, std::string_view key) noexcept {
    Hash64 h;
    h.update("cdip-session-key", 16);
    h.update(session.b.data(), 16);
    h.u64(key.size());
    h.update(key.data(), key.size());
    return h.finish();
}

const char* rejectReasonName(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None: return "none";
        case RejectReason::SampleRate: return "incompatible sample rate";
        case RejectReason::Channels: return "unsupported channel count";
        case RejectReason::RailModel: return "different rail model (rails cannot be aggregated)";
        case RejectReason::NominalRail: return "different nominal rail voltage";
        case RejectReason::PsuManifest: return "different PSU manifest";
        case RejectReason::Epoch: return "different timeline epoch";
        case RejectReason::BadRequest: return "malformed join request";
        case RejectReason::Evicted: return "evicted (lease expired or ring corrupt)";
        case RejectReason::BrokerClosed: return "broker closed";
        case RejectReason::Full: return "group full";
    }
    return "unknown";
}

const char* regionErrorName(RegionError e) noexcept {
    static const char* const names[] = {"none", "region too small", "bad magic", "incompatible major version",
                                        "bad header size", "header hash mismatch", "bad member count", "bad ring size",
                                        "bad quantum", "bad channel count", "bad sample rate", "layout mismatch",
                                        "total size mismatch", "bad group name", "bad latency", "session key mismatch",
                                        "group mismatch"};
    const auto i = static_cast<std::uint32_t>(e);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "unknown";
}

bool computeLayout(std::uint32_t maxMembers, Layout& out) noexcept {
    if (maxMembers < kMinMembers || maxMembers > kMaxMembers) return false;
    Layout l;
    l.maxMembers = maxMembers;
    l.entryOffset = sizeof(RegionHeader);
    l.entryStride = sizeof(MemberEntry);
    l.ringsOffset = align64(l.entryOffset + std::uint64_t(maxMembers) * l.entryStride);
    l.upRingOffset = 0;
    l.downRingOffset = align64(sizeof(RingCtl) + std::uint64_t(kRingSlots) * sizeof(UpFrame));
    l.memberStride = align64(l.downRingOffset + sizeof(RingCtl) + std::uint64_t(kRingSlots) * sizeof(DownFrame));
    l.totalBytes = l.ringsOffset + std::uint64_t(maxMembers) * l.memberStride;
    out = l;
    return true;
}

RegionError validateRegion(const std::byte* base, std::size_t mappedBytes, Layout& layout) noexcept {
    if (base == nullptr || mappedBytes < sizeof(RegionHeader)) return RegionError::TooSmall;
    RegionHeader h;
    std::memcpy(&h, base, sizeof h);   // private copy: no TOCTOU on shared bytes
    if (std::memcmp(h.magic, "CDIP", 4) != 0) return RegionError::Magic;
    if (h.major != kProtocolMajor) return RegionError::Version;
    if (h.headerBytes != sizeof(RegionHeader)) return RegionError::HeaderBytes;
    if (hash64(&h, offsetof(RegionHeader, immutableHash)) != h.immutableHash) return RegionError::HeaderHash;
    if (h.maxMembers < kMinMembers || h.maxMembers > kMaxMembers) return RegionError::Members;
    if (h.ringSlots != kRingSlots) return RegionError::Ring;
    if (h.quantum != kQuantum) return RegionError::Quantum;
    if (h.channels < 1 || h.channels > kMaxChannels) return RegionError::Channels;
    if (!std::isfinite(h.sampleRate) || h.sampleRate < 8000.0 || h.sampleRate > 768000.0) return RegionError::SampleRate;
    if (h.latencyQuanta < 1 || h.latencyQuanta > kRingSlots / 2) return RegionError::Latency;
    if (!std::isfinite(h.nominalRailV) || h.nominalRailV <= 0) return RegionError::SampleRate;
    bool nul = false;
    for (char c : h.groupName) if (c == '\0') { nul = true; break; }
    if (!nul) return RegionError::Name;
    Layout l;
    if (!computeLayout(h.maxMembers, l)) return RegionError::Members;
    if (h.entryOffset != l.entryOffset || h.entryStride != l.entryStride || h.ringsOffset != l.ringsOffset ||
        h.memberStride != l.memberStride || h.upFrameBytes != sizeof(UpFrame) || h.downFrameBytes != sizeof(DownFrame))
        return RegionError::Layout;
    if (h.totalBytes != l.totalBytes || l.totalBytes > mappedBytes || l.totalBytes > kMaxRegionBytes) return RegionError::TotalBytes;
    layout = l;
    return RegionError::None;
}

void ShmRing::resetShared() noexcept {
    if (!ctl_) return;
    storeRelease(ctl_->tail, 0);
    storeRelease(ctl_->head, 0);
    resetLocal();
}

bool ShmRing::push(const void* frame) noexcept {
    if (!ctl_ || corrupt_) return false;
    if (local_ - cached_ >= kRingSlots) {
        const std::uint64_t head = loadAcquire(ctl_->head);
        if (local_ - head > kRingSlots) { corrupt_ = true; return false; }   // head ahead of tail/ too far behind
        cached_ = head;
        if (local_ - cached_ >= kRingSlots) return false;                     // full
    }
    std::memcpy(slots_ + (local_ & (kRingSlots - 1)) * frameBytes_, frame, frameBytes_);
    ++local_;
    storeRelease(ctl_->tail, local_);
    return true;
}

bool ShmRing::peek(void* out) noexcept {
    if (!ctl_ || corrupt_) return false;
    if (local_ == cached_) {
        const std::uint64_t tail = loadAcquire(ctl_->tail);
        if (tail - local_ > kRingSlots) { corrupt_ = true; return false; }
        cached_ = tail;
        if (local_ == cached_) return false;
    }
    std::memcpy(out, slots_ + (local_ & (kRingSlots - 1)) * frameBytes_, frameBytes_);
    return true;
}

void ShmRing::pop() noexcept {
    if (!ctl_ || corrupt_ || local_ == cached_) return;
    ++local_;
    storeRelease(ctl_->head, local_);
}

void fillEnvelope(Envelope& e, PayloadType t, std::uint32_t frameBytes, std::uint32_t payloadBytes,
                  const RegionHeader& h, const Uuid& member, std::uint64_t generation, std::uint32_t channels,
                  std::int64_t sampleStart, std::uint64_t sequence) noexcept {
    std::memcpy(e.magic, "CDIP", 4);
    e.major = kProtocolMajor;
    e.minor = kProtocolMinor;
    e.byteSize = frameBytes;
    e.payloadType = static_cast<std::uint32_t>(t);
    e.payloadLength = payloadBytes;
    e.channels = channels;
    std::memcpy(e.group, h.group, 16);
    std::memcpy(e.session, h.session, 16);
    std::memcpy(e.member, member.b.data(), 16);
    e.generation = generation;
    e.sampleRate = h.sampleRate;
    e.timelineEpoch = h.timelineEpoch;
    e.sampleStart = sampleStart;
    e.sequence = sequence;
    e.reserved[0] = e.reserved[1] = 0;
}

bool envelopeValid(const Envelope& e, PayloadType t, std::uint32_t frameBytes, std::uint32_t payloadBytes,
                   const RegionHeader& h, const Uuid& member, std::uint64_t generation) noexcept {
    if (std::memcmp(e.magic, "CDIP", 4) != 0 || e.major != kProtocolMajor) return false;
    if (e.byteSize != frameBytes || e.payloadType != static_cast<std::uint32_t>(t) || e.payloadLength != payloadBytes) return false;
    if (e.channels < 1 || e.channels > kMaxChannels) return false;
    if (std::memcmp(e.group, h.group, 16) != 0 || std::memcmp(e.session, h.session, 16) != 0) return false;
    if (std::memcmp(e.member, member.b.data(), 16) != 0) return false;
    if (e.generation != generation) return false;                       // stale occupancy
    if (!(e.sampleRate == h.sampleRate) || e.timelineEpoch != h.timelineEpoch) return false;
    if (e.sampleStart < 0 || e.sampleStart % std::int64_t(kQuantum) != 0) return false;
    return true;
}

}  // namespace cd::network
