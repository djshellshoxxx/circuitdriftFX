// CircuitDrift network: instance-network protocol envelope and POD payloads (P15, CD-F11/T11).
// Owner: cd_network. Spec: docs/planned-features/04-instance-network.md (D17).
// Everything here is framework/OS independent. The real per-user broker PROCESS and OS shared
// memory are platform adapters (gate G05, open); in this repository they are abstracted behind
// RingAllocator/SharedRegion (PreparedRing.h) and exercised against in-process mocks only.
//
// Wire layout (FrameHeader, fixed, little-endian hosts assumed; every field naturally aligned, no
// implicit padding, sizeof == 112, alignof == 8; offsets are static_asserted below):
//    0 magic[4] "CDIP"     4 major u16          6 minor u16         8 byteSize u32 (header+payload)
//   12 payloadType u32    16 group  Uuid128    32 session Uuid128  48 member Uuid128
//   64 generation u64     72 sampleRate f64    80 channels u32     84 payloadLength u32
//   88 epoch u64          96 sampleStart u64  104 sequence u64
// Payloads are POD with no pointers. A frame is header + payload in one trivially copyable struct
// and is only ever consumed after validate() accepted its header.
// Timing labels: live coupling is delayed by kLiveLatencyQuanta quanta (1024 samples, displayed as
// 1024/Fs seconds) and is labelled "approximate live coupling". It is never sample-perfect under
// arbitrary DAW scheduling; exact offline equivalence only comes from CoRender (CoRender.h).
#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <random>
#include <type_traits>

namespace cd::network {

inline constexpr std::uint32_t kQuantumFrames = 128;      // one group tick
inline constexpr std::uint32_t kRingQuanta = 64;          // slots per member/direction ring
inline constexpr std::uint32_t kMaxChannels = 2;          // mono or stereo members
inline constexpr std::uint32_t kLiveLatencyQuanta = 8;    // L: consume broker result for k-L only
inline constexpr std::uint32_t kMaxMembersPerGroup = 32;
inline constexpr std::uint32_t kMaxGroupsPerBroker = 8;
inline constexpr double kTickDeadlineSeconds = 0.050;     // broker forced-advance deadline
inline constexpr double kDemandDecayTauSeconds = 0.100;   // missing-demand decay time constant
inline constexpr double kInfluenceRampSeconds = 0.050;    // join/leave/broker-loss ramps
inline constexpr double kHeartbeatTimeoutSeconds = 0.500; // member removal
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr char kLiveCouplingLabel[] = "approximate live coupling";

// Display delay of the live path (bleed and shared demand): L*128/Fs seconds (1024/Fs).
constexpr double liveDisplayDelaySeconds(double sampleRate) noexcept {
    return sampleRate > 0 ? double(kLiveLatencyQuanta * kQuantumFrames) / sampleRate : 0.0;
}

// 128-bit identifier (group, session, member). Generated off-audio only.
struct Uuid128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    bool isNil() const noexcept { return hi == 0 && lo == 0; }
    friend bool operator==(const Uuid128&, const Uuid128&) = default;
    friend auto operator<=>(const Uuid128&, const Uuid128&) = default;
};
// Random RFC 4122 version-4 layout from the given engine (non-audio).
Uuid128 makeUuidV4(std::mt19937_64& rng) noexcept;

enum class PayloadType : std::uint32_t { Demand = 1, Result = 2 };

struct FrameHeader {
    char magic[4] = {'C', 'D', 'I', 'P'};
    std::uint16_t major = kProtocolMajor;
    std::uint16_t minor = kProtocolMinor;
    std::uint32_t byteSize = 0;       // sizeof(FrameHeader) + payloadLength
    std::uint32_t payloadType = 0;    // PayloadType
    Uuid128 group;
    Uuid128 session;                  // ephemeral session token of the group
    Uuid128 member;                   // producer (Demand) or receiver (Result)
    std::uint64_t generation = 0;     // roster generation
    double sampleRate = 0;
    std::uint32_t channels = 0;       // 1|2
    std::uint32_t payloadLength = 0;
    std::uint64_t epoch = 0;          // host transport/timeline epoch (seek/loop/stop bump it)
    std::uint64_t sampleStart = 0;    // absolute timeline sample of the quantum start (tick*128)
    std::uint64_t sequence = 0;       // producer-monotonic frame counter
};
static_assert(std::is_trivially_copyable_v<FrameHeader> && std::is_standard_layout_v<FrameHeader>);
static_assert(sizeof(Uuid128) == 16 && alignof(Uuid128) == 8);
static_assert(sizeof(FrameHeader) == 112 && alignof(FrameHeader) == 8, "fixed wire layout");
static_assert(offsetof(FrameHeader, byteSize) == 8 && offsetof(FrameHeader, payloadType) == 12);
static_assert(offsetof(FrameHeader, group) == 16 && offsetof(FrameHeader, session) == 32 &&
              offsetof(FrameHeader, member) == 48 && offsetof(FrameHeader, generation) == 64);
static_assert(offsetof(FrameHeader, sampleRate) == 72 && offsetof(FrameHeader, channels) == 80 &&
              offsetof(FrameHeader, payloadLength) == 84 && offsetof(FrameHeader, epoch) == 88 &&
              offsetof(FrameHeader, sampleStart) == 96 && offsetof(FrameHeader, sequence) == 104);

// Member -> broker, once per quantum: supply demand plus the PRE-CIRCUIT audio tap (after input
// trim, before the member's circuit and before any received bleed: no recursive feedback).
struct DemandPayload {
    double currentA = 0;       // supply current demand [A]
    double powerW = 0;         // supply power [W]
    double activity = 0;       // normalized activity 0..1
    std::uint64_t railModelId = 0;
    std::uint32_t frames = kQuantumFrames;
    std::uint32_t flags = 0;
    double tap[kMaxChannels][kQuantumFrames] = {};
};

inline constexpr std::uint32_t kResultDegraded = 1u;   // tick advanced with missing members

// Broker -> one receiver, once per completed tick: shared rail readout and that receiver's bleed.
struct ResultPayload {
    double railFraction = 1;   // r = V / V_nom (readout)
    double railVolts = 0;
    double totalCurrentA = 0;
    std::uint32_t flags = 0;
    std::uint32_t activeMembers = 0;
    std::uint32_t presentMembers = 0;
    std::uint32_t frames = kQuantumFrames;
    double bleed[kMaxChannels][kQuantumFrames] = {};
};

struct DemandFrame { FrameHeader header; DemandPayload payload; };
struct ResultFrame { FrameHeader header; ResultPayload payload; };
static_assert(std::is_trivially_copyable_v<DemandFrame> && std::is_standard_layout_v<DemandFrame>);
static_assert(std::is_trivially_copyable_v<ResultFrame> && std::is_standard_layout_v<ResultFrame>);
static_assert(offsetof(DemandFrame, payload) == sizeof(FrameHeader));
static_assert(offsetof(ResultFrame, payload) == sizeof(FrameHeader));

template <class P> constexpr PayloadType payloadTypeOf() noexcept;
template <> constexpr PayloadType payloadTypeOf<DemandPayload>() noexcept { return PayloadType::Demand; }
template <> constexpr PayloadType payloadTypeOf<ResultPayload>() noexcept { return PayloadType::Result; }

enum class FrameError : std::uint32_t {
    None = 0, BadMagic, IncompatibleMajor, InvalidSize, WrongPayloadType, WrongGroup, WrongSession,
    WrongMember, SampleRateMismatch, EpochMismatch, StaleGeneration, InvalidChannels, Count
};
const char* toString(FrameError e) noexcept;

// What the receiver requires before it consumes a frame.
struct FrameExpectation {
    PayloadType type = PayloadType::Demand;
    Uuid128 group;
    Uuid128 session;
    Uuid128 member;                 // nil = any member
    double sampleRate = 0;
    std::uint64_t epoch = 0;
    std::uint64_t minGeneration = 0;
};

// Pure, noexcept, audio-safe. Rejects (in this order) bad magic, incompatible major (minor may
// differ), wrong payload type, invalid byte/payload size for the type, wrong group/session/member, mismatched Fs,
// mismatched epoch, stale generation (< minGeneration) and channel counts other than 1|2.
FrameError validate(const FrameHeader& h, const FrameExpectation& x) noexcept;

// Fills the identity fields of a header for payload type P (sizes, type, magic, version).
template <class P>
void stampHeader(FrameHeader& h) noexcept {
    h = FrameHeader{};
    h.payloadType = static_cast<std::uint32_t>(payloadTypeOf<P>());
    h.payloadLength = static_cast<std::uint32_t>(sizeof(P));
    h.byteSize = static_cast<std::uint32_t>(sizeof(FrameHeader) + sizeof(P));
}

}  // namespace cd::network
