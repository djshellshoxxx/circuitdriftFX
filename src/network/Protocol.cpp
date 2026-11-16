// CircuitDrift network: protocol validation (see Protocol.h).
#include "network/Protocol.h"

namespace cd::network {

Uuid128 makeUuidV4(std::mt19937_64& rng) noexcept {
    Uuid128 u{rng(), rng()};
    u.hi = (u.hi & ~std::uint64_t(0xF000)) | std::uint64_t(0x4000);          // version 4
    u.lo = (u.lo & ~(std::uint64_t(0xC) << 60)) | (std::uint64_t(0x8) << 60); // RFC 4122 variant
    return u;
}

const char* toString(FrameError e) noexcept {
    switch (e) {
        case FrameError::None: return "ok";
        case FrameError::BadMagic: return "bad magic (not CDIP)";
        case FrameError::IncompatibleMajor: return "incompatible protocol major version";
        case FrameError::InvalidSize: return "invalid byte/payload size";
        case FrameError::WrongPayloadType: return "unexpected payload type";
        case FrameError::WrongGroup: return "frame belongs to another group";
        case FrameError::WrongSession: return "frame belongs to another session";
        case FrameError::WrongMember: return "frame addressed to another member";
        case FrameError::SampleRateMismatch: return "sample rate mismatch";
        case FrameError::EpochMismatch: return "timeline epoch mismatch (stale after seek/loop)";
        case FrameError::StaleGeneration: return "stale roster generation";
        case FrameError::InvalidChannels: return "channel count must be 1 or 2";
        case FrameError::Count: break;
    }
    return "unknown";
}

namespace {
std::uint32_t payloadSizeFor(PayloadType t) noexcept {
    switch (t) {
        case PayloadType::Demand: return sizeof(DemandPayload);
        case PayloadType::Result: return sizeof(ResultPayload);
    }
    return 0;
}
}  // namespace

FrameError validate(const FrameHeader& h, const FrameExpectation& x) noexcept {
    if (h.magic[0] != 'C' || h.magic[1] != 'D' || h.magic[2] != 'I' || h.magic[3] != 'P') return FrameError::BadMagic;
    if (h.major != kProtocolMajor) return FrameError::IncompatibleMajor;
    if (h.payloadType != static_cast<std::uint32_t>(x.type)) return FrameError::WrongPayloadType;
    const std::uint32_t want = payloadSizeFor(x.type);
    if (h.payloadLength != want || h.byteSize != sizeof(FrameHeader) + want) return FrameError::InvalidSize;
    if (h.group != x.group) return FrameError::WrongGroup;
    if (h.session != x.session) return FrameError::WrongSession;
    if (!x.member.isNil() && h.member != x.member) return FrameError::WrongMember;
    if (!(h.sampleRate == x.sampleRate)) return FrameError::SampleRateMismatch;
    if (h.epoch != x.epoch) return FrameError::EpochMismatch;
    if (h.generation < x.minGeneration) return FrameError::StaleGeneration;
    if (h.channels != 1 && h.channels != 2) return FrameError::InvalidChannels;
    return FrameError::None;
}

}  // namespace cd::network
