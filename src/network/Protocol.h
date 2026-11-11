// CircuitDrift network: CDIP wire/shared-memory protocol for the instance network (CD-F11, P15).
// Owner: cd_network. Pure POD layout + validation helpers; no allocation, no I/O.
//
// Everything stored in a shared region is plain data with no pointers; offsets are relative to the
// region base. Every field read from shared memory is treated as HOSTILE: the immutable header is
// covered by a hash and every derived offset/stride is recomputed locally and must match exactly;
// mutable fields (ring indices, entry states, frames) are validated on every read and frames are
// copied to private memory before validation (no time-of-check/time-of-use on shared bytes).
//
// Cross-process atomics: only 64-bit naturally aligned words accessed through std::atomic_ref,
// which must be always-lock-free (address-free on the supported Linux/macOS ABIs). Other
// std::atomic types are never placed in shared memory.
//
// Hash: CDH64 = FNV-1a 64 followed by a splitmix64 finaliser. NON-CRYPTOGRAPHIC: identity and
// accidental-corruption detection only. It is not authentication; OS object permissions (0600,
// owner check) are the security boundary for local IPC.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace cd::network {

inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::uint32_t kQuantum = 128;          // frames per group tick
inline constexpr std::uint32_t kRingSlots = 64;         // fixed 64-quantum rings per member/direction
inline constexpr std::uint32_t kMaxMembers = 32;
inline constexpr std::uint32_t kMinMembers = 2;
inline constexpr std::uint32_t kMaxGroupsPerBroker = 8;
inline constexpr std::uint32_t kMaxChannels = 2;
inline constexpr std::uint32_t kDefaultLatencyQuanta = 8;   // L = 8 quanta = 1024 samples
inline constexpr double kDeadlineMs = 50.0;             // broker tick deadline
inline constexpr double kMissingDemandTauS = 0.100;     // missing input: last demand decays, 100 ms tau
inline constexpr double kInfluenceRampS = 0.050;        // join/leave/broker-loss ramps
inline constexpr double kLeaseMs = 500.0;               // heartbeat lease
inline constexpr double kTapLimit = 16.0;               // |tap| containment (+24 dBFS); non-finite -> 0
inline constexpr std::size_t kMaxRegionBytes = std::size_t(64) << 20;

static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free, "IPC needs lock-free 64-bit atomic_ref");
static_assert(std::atomic_ref<std::uint64_t>::required_alignment <= alignof(std::uint64_t), "IPC word alignment");

// ---- identity ----
struct Uuid {
    std::array<std::uint8_t, 16> b{};
    bool operator==(const Uuid&) const = default;
    bool isNil() const noexcept;
    static Uuid random();                         // std::random_device; non-audio
    static Uuid fromSeed(std::uint64_t seed) noexcept;   // deterministic (tests, co-render ids)
};
bool uuidLess(const Uuid& a, const Uuid& b) noexcept;

// CDH64 (see header comment). Non-cryptographic.
class Hash64 {
public:
    void update(const void* p, std::size_t n) noexcept;
    void u64(std::uint64_t v) noexcept { update(&v, sizeof v); }
    void f64(double v) noexcept { std::uint64_t u; std::memcpy(&u, &v, 8); u64(u); }
    std::uint64_t finish() const noexcept;
private:
    std::uint64_t h_ = 0xcbf29ce484222325ull;
};
std::uint64_t hash64(const void* p, std::size_t n) noexcept;
std::uint64_t sessionKeyHash(const Uuid& session, std::string_view key) noexcept;

// ---- frames ----
enum class PayloadType : std::uint32_t { Up = 1, Down = 2 };

struct Envelope {                       // 128 bytes, no padding
    char magic[4];                      // "CDIP"
    std::uint16_t major, minor;
    std::uint32_t byteSize;             // whole frame
    std::uint32_t payloadType;
    std::uint32_t payloadLength;
    std::uint32_t channels;
    std::uint8_t group[16], session[16], member[16];
    std::uint64_t generation;           // member entry generation (slot occupancy)
    double sampleRate;
    std::uint64_t timelineEpoch;
    std::int64_t sampleStart;           // absolute timeline sample of frame 0 (quantum aligned)
    std::uint64_t sequence;
    std::uint64_t reserved[2];
};
static_assert(sizeof(Envelope) == 128 && offsetof(Envelope, generation) == 72);   // no padding

struct UpPayload {                      // member -> broker, one quantum
    double demandA;                     // supply current demand [A] (synthetic demand model)
    double powerW;                      // power [W]
    double activity;                    // normalized activity [0,1]
    std::uint64_t railModelId;
    std::uint64_t reserved[4];
    double tap[kMaxChannels][kQuantum]; // audio tap (after input trim, before circuit/received bleed)
};
struct DownPayload {                    // broker -> member, result of group tick `tick`
    std::int64_t tick;                  // quantum index (sampleStart / kQuantum)
    std::uint64_t rosterGeneration;
    double railV;                       // shared reservoir rail [V] (synthetic model)
    std::uint32_t flags;                // tick flags (see tickflag)
    std::uint32_t activeMembers;
    std::uint64_t reserved[4];
    double bleed[kMaxChannels][kQuantum];
};
namespace tickflag {
inline constexpr std::uint32_t kDegraded = 1u << 0;   // a member frame was missing (deadline)
inline constexpr std::uint32_t kRowClamped = 1u << 1; // topology row clamped to the bleed gain
}
struct UpFrame { Envelope env; UpPayload p; };
struct DownFrame { Envelope env; DownPayload p; };
static_assert(sizeof(UpFrame) % 64 == 0 && sizeof(DownFrame) % 64 == 0);

// ---- region layout ----
struct RingCtl {                        // 128 bytes: tail and head on separate cache lines
    std::uint64_t tail; std::uint64_t padT[7];
    std::uint64_t head; std::uint64_t padH[7];
};
static_assert(sizeof(RingCtl) == 128);

enum class EntryState : std::uint64_t { Free = 0, Claimed = 1, Requested = 2, Active = 3, Rejected = 4, Leaving = 5 };
enum class RejectReason : std::uint64_t {
    None = 0, SampleRate, Channels, RailModel, NominalRail, PsuManifest, Epoch, BadRequest, Evicted, BrokerClosed, Full
};
const char* rejectReasonName(RejectReason r) noexcept;

struct MemberEntry {                    // 256 bytes
    std::uint64_t state;                // EntryState (atomic)
    std::uint64_t generation;           // set by broker on activation (atomic)
    std::uint64_t heartbeat;            // member increments (atomic)
    std::uint64_t reason;               // RejectReason (atomic)
    std::uint8_t member[16];
    double sampleRate;
    std::uint64_t timelineEpoch;
    std::uint64_t railModelId;
    double nominalRailV;
    std::uint64_t psuManifestHash;
    std::uint32_t channels;
    std::uint32_t reserved0;
    std::uint64_t reserved[20];
};
static_assert(sizeof(MemberEntry) == 256);

enum class BrokerState : std::uint64_t { Starting = 0, Running = 1, Closed = 2 };

struct RegionHeader {                   // 512 bytes; [0, offsetof(immutableHash)) is immutable
    char magic[4];
    std::uint16_t major, minor;
    std::uint32_t headerBytes;
    std::uint32_t latencyQuanta;
    std::uint64_t totalBytes;
    std::uint8_t group[16], session[16];
    std::uint32_t maxMembers, ringSlots, quantum, channels;
    double sampleRate;
    std::uint64_t timelineEpoch;
    std::uint64_t entryOffset, entryStride, ringsOffset, memberStride, upFrameBytes, downFrameBytes;
    std::uint64_t railModelId;
    double nominalRailV;
    std::uint64_t psuManifestHash;
    std::uint64_t sessionKeyHash;
    std::uint64_t bleedSeed;
    char groupName[64];
    std::uint64_t immutableHash;        // CDH64 of the bytes before this field
    std::uint64_t pad0[1];
    // mutable words, one cache line each (atomic access only)
    std::uint64_t rosterGeneration; std::uint64_t pad1[7];
    std::uint64_t brokerHeartbeat;  std::uint64_t pad2[7];
    std::uint64_t brokerState;      std::uint64_t pad3[7];
    std::uint64_t pad4[8];
};
static_assert(sizeof(RegionHeader) == 512 && offsetof(RegionHeader, rosterGeneration) == 256);

struct Layout {
    std::uint32_t maxMembers = 0;
    std::uint64_t entryOffset = 0, entryStride = 0, ringsOffset = 0, memberStride = 0;
    std::uint64_t upRingOffset = 0, downRingOffset = 0;    // within a member block
    std::uint64_t totalBytes = 0;
};
// Computes the canonical layout for maxMembers in [kMinMembers, kMaxMembers]; false otherwise.
bool computeLayout(std::uint32_t maxMembers, Layout& out) noexcept;

// Reasons an attach/validation rejects a region.
enum class RegionError : std::uint32_t {
    None = 0, TooSmall, Magic, Version, HeaderBytes, HeaderHash, Members, Ring, Quantum, Channels,
    SampleRate, Layout, TotalBytes, Name, Latency, SessionKey, Group
};
const char* regionErrorName(RegionError e) noexcept;
// Validates the header of a mapped region of `mappedBytes` (hostile input). On success fills
// `layout` from LOCAL computation (never from the shared offsets, which must merely match).
RegionError validateRegion(const std::byte* base, std::size_t mappedBytes, Layout& layout) noexcept;

// ---- atomic word access on shared memory ----
inline std::uint64_t loadAcquire(std::uint64_t& w) noexcept { return std::atomic_ref<std::uint64_t>(w).load(std::memory_order_acquire); }
inline std::uint64_t loadRelaxed(std::uint64_t& w) noexcept { return std::atomic_ref<std::uint64_t>(w).load(std::memory_order_relaxed); }
inline void storeRelease(std::uint64_t& w, std::uint64_t v) noexcept { std::atomic_ref<std::uint64_t>(w).store(v, std::memory_order_release); }
inline std::uint64_t fetchAdd(std::uint64_t& w, std::uint64_t v) noexcept { return std::atomic_ref<std::uint64_t>(w).fetch_add(v, std::memory_order_acq_rel); }
inline bool compareExchange(std::uint64_t& w, std::uint64_t expected, std::uint64_t desired) noexcept {
    return std::atomic_ref<std::uint64_t>(w).compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
}

// Bounded wait-free SPSC ring living in shared memory. Each side keeps its own index privately and
// only READS the other side's index from shared memory, so a corrupted index can at worst mark the
// ring corrupt (slot addressing is masked: never out of bounds). No spin, no retry loop.
class ShmRing {
public:
    ShmRing() = default;
    ShmRing(std::byte* ctl, std::byte* slots, std::uint32_t frameBytes) noexcept
        : ctl_(reinterpret_cast<RingCtl*>(ctl)), slots_(slots), frameBytes_(frameBytes) {}
    bool valid() const noexcept { return ctl_ != nullptr; }
    void resetShared() noexcept;                         // owner (broker) on activation
    void resetLocal() noexcept { local_ = 0; cached_ = 0; corrupt_ = false; }
    bool corrupt() const noexcept { return corrupt_; }
    // Producer: copies frameBytes from `frame`; false when full or corrupt.
    bool push(const void* frame) noexcept;
    // Consumer: copies the head frame into `out` (frameBytes); false when empty or corrupt.
    bool peek(void* out) noexcept;
    void pop() noexcept;
private:
    RingCtl* ctl_ = nullptr;
    std::byte* slots_ = nullptr;
    std::uint32_t frameBytes_ = 0;
    std::uint64_t local_ = 0;    // producer: tail; consumer: head
    std::uint64_t cached_ = 0;   // last observed opposite index
    bool corrupt_ = false;
};

// Envelope helpers.
void fillEnvelope(Envelope& e, PayloadType t, std::uint32_t frameBytes, std::uint32_t payloadBytes,
                  const RegionHeader& h, const Uuid& member, std::uint64_t generation, std::uint32_t channels,
                  std::int64_t sampleStart, std::uint64_t sequence) noexcept;
// Rejects incompatible major, wrong magic/size/type/length, foreign group/session/member,
// mismatched Fs/epoch, unaligned start or stale generation (before any payload consumption).
bool envelopeValid(const Envelope& e, PayloadType t, std::uint32_t frameBytes, std::uint32_t payloadBytes,
                   const RegionHeader& h, const Uuid& member, std::uint64_t generation) noexcept;

}  // namespace cd::network
