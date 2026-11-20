// CircuitDrift network: multi-instance shared PSU and audio bleed (CD-F11, P15; 04 spec; gate G05).
//
// Model
//   * A Group is a POD SharedLayout (header + 32 member slots, each with a 64-quantum frame ring)
//     that lives either on the heap (in-process Registry) or in OS shared memory (SharedMemory.h).
//   * Join is opt-in, worker-side. A group is identified by random 128-bit UUID *and* an explicit
//     session-key hash: the same UUID with another session key is a different, isolated group.
//     At most 32 members per group, 8 groups per Registry (broker). Members must share sample
//     rate and PSU rail model/nominal volts; otherwise join is rejected with a per-member reason.
//   * Every 128-sample quantum a Member publishes a frame (supply demand [A], power [W], activity,
//     optional stereo audio tap) into its own ring slot (tick % 64) and consumes the completed frames
//     of its peers for tick-L (L = latencyQuanta, live default 8). Consumption never waits: a missing,
//     late, torn, stale-generation, wrong-epoch or hostile frame is treated as absent: audio
//     contributes zero and demand holds the last value decayed with tau = 100 ms; the result is
//     flagged Degraded. Joining/leaving influence is ramped over 50 ms.
//   * Audio path contract (publish/consume/accessors below): noexcept, no allocation, no locks, no
//     system calls, bounded work (<= 32 peers x one 128-frame copy), single seqlock-style check per
//     frame with NO retry loop. All shared fields are std::atomic with relaxed ordering around a
//     release/acquire stamp, so there is no data race even with a hostile writer.
//   * Bleed: c_i = g/max(1,N-1) * sum_{j!=i} p_j x_j with p_j a fixed seeded sign, g = 10^(dB/20)
//     (dB in [-90,-24] or Off). An optional topology row replaces the weights; its absolute row sum
//     is scaled down so that it never exceeds g.
//   * Cross-process: the layout is plain atomics of lock-free 32/64-bit integers; the header carries
//     magic "CDIP", major/minor, byte size, group/session ids, sample rate and shape constants.
//     validateLayout() must pass before anything is dereferenced; all indices on the audio path are
//     derived locally (never read from shared memory). Cross-process atomics need a layout/ABI both
//     processes share (same compiler ABI / architecture); mixed-ABI hosts are not supported.
//   * Not sample-perfect under arbitrary DAW scheduling. Deterministic offline coupling is the
//     influence-bundle harness (InfluenceBundle.h), not this live path.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cd::network {

constexpr std::uint32_t kMagic = 0x50494443u;  // bytes "CDIP"
constexpr std::uint16_t kProtocolMajor = 1, kProtocolMinor = 0;
constexpr unsigned kMaxMembers = 32, kMaxGroups = 8;
constexpr unsigned kQuantumFrames = 128, kRingQuanta = 64, kAudioChannels = 2;
constexpr unsigned kLiveLatencyQuanta = 8;           // 1024 samples
constexpr double kHeartbeatTimeoutMs = 500.0, kDecayTauMs = 100.0, kRampMs = 50.0;
constexpr double kBleedMinDb = -90.0, kBleedMaxDb = -24.0;
constexpr float kMaxDemandA = 1.0e4f, kMaxAudioAbs = 16.0f;
constexpr float kMaxNominalRailV = 10000.0f;  // shared and in-process header admission bound  // sanitising bounds for peer data

struct Uuid128 {
    std::uint64_t hi = 0, lo = 0;
    friend bool operator==(const Uuid128&, const Uuid128&) = default;
    friend bool operator<(const Uuid128& a, const Uuid128& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
};

struct FrameSlot {
    std::atomic<std::uint64_t> stamp{0};  // tick+1 once complete; 0 while being written
    std::atomic<std::uint32_t> epoch{0}, generation{0};
    std::atomic<std::uint32_t> demandBits{0}, powerBits{0}, activityBits{0}, hasAudio{0};
    std::atomic<std::uint32_t> audio[kAudioChannels * kQuantumFrames];  // float bit patterns
};
struct MemberSlot {
    std::atomic<std::uint32_t> state{0};      // 0 free, 1 claiming, 2 active
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::uint64_t> idHi{0}, idLo{0};
    std::atomic<std::uint64_t> heartbeat{0};
    std::atomic<std::uint32_t> railModelId{0};
    FrameSlot ring[kRingQuanta];
};
struct SharedHeader {
    std::uint32_t magic = 0;
    std::uint16_t major = 0, minor = 0;
    std::uint64_t byteSize = 0;
    std::uint64_t groupHi = 0, groupLo = 0, sessionHash = 0;
    double sampleRate = 0;
    std::uint32_t channels = 0, slotCount = 0, ringQuanta = 0, quantumFrames = 0;
    std::uint32_t railModelId = 0;
    float nominalRailV = 0;
    std::uint64_t bleedSeed = 0;
    std::atomic<std::uint64_t> rosterGeneration{0};
    std::atomic<std::uint32_t> activeMask{0};
    std::atomic<std::uint32_t> epoch{0};      // timeline epoch; bump on seek/loop/transport reset
};
struct SharedLayout {
    SharedHeader header;
    MemberSlot members[kMaxMembers];
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free,
              "cross-process shared state requires lock-free atomics");
static_assert(sizeof(std::atomic<std::uint32_t>) == 4 && sizeof(std::atomic<std::uint64_t>) == 8, "unexpected atomic layout");

struct GroupSpec {
    Uuid128 groupId;
    std::uint64_t sessionKeyHash = 0;  // hash of the explicit session key (never the key itself)
    double sampleRate = 48000.0;
    std::uint32_t channels = 2;
    std::uint32_t railModelId = 1;
    float nominalRailV = 0.0f;
    std::uint64_t bleedSeed = 0x43444946u;
};

enum class JoinResult : unsigned char {
    Ok, InvalidArgument, GroupLimit, MemberLimit, IncompatibleSampleRate, IncompatibleRail, Duplicate
};
const char* joinResultText(JoinResult r) noexcept;

// Structural validation of a mapped region (hostile-safe). `size` is the mapped byte count.
bool validateLayout(const void* mem, std::size_t size, std::string& error);
// Placement-initialises a zeroed region as a fresh group. `size` must be >= sizeof(SharedLayout).
SharedLayout* initializeLayout(void* mem, std::size_t size, const GroupSpec& spec, std::string& error);
// Fills the header of an already constructed (zero/default-initialised) layout.
void fillHeader(SharedLayout& layout, const GroupSpec& spec) noexcept;

enum class MemberStatus : unsigned char { Coupled, Degraded, Unavailable, Removed };

struct MemberFrameIn {
    float demandA = 0, powerW = 0, activity = 0;
    const float* left = nullptr;   // 128 samples or null (no audio)
    const float* right = nullptr;  // null with left set = mono: duplicated into both channels at -3 dB
};
struct ConsumeOut {
    float sharedDemandA = 0;       // own last demand + coupling * sum(peer demand), shared PSU aggregation
    float sharedPowerW = 0;
    std::uint32_t members = 0;     // active members in roster (including this one)
    std::uint32_t missingPeers = 0;
    MemberStatus status = MemberStatus::Coupled;
    float coupling = 1.0f;         // 0..1 ramp (fade to local PSU / zero bleed when 0)
    float bleedRowAbsSum = 0;      // sum |w_ij| actually applied (<= configured gain g)
};

// Pure helper (also used by the co-render harness): per-receiver bleed row. ids/mask describe the
// roster; topology may be null (default weights). Returns sum |w|. w has kMaxMembers entries.
double bleedRowWeights(unsigned self, const Uuid128* ids, std::uint32_t activeMask, std::uint64_t seed,
                       double gainLinear, const float* topologyRow, float* w) noexcept;
double bleedGainLinear(bool enabled, double dB) noexcept;  // Off -> 0; dB clamped to [-90,-24]
float bleedSign(std::uint64_t seed, const Uuid128& id) noexcept;  // +1 or -1, fixed per (seed, id)

class Member {
public:
    // ---- audio thread: noexcept, no alloc, no locks, never waits ----
    // Publishes this quantum's frame. Returns false if the member was removed (then needs rejoin).
    bool publish(std::uint64_t tick, const MemberFrameIn& in) noexcept;
    // Reads peers' frames for tick - latencyQuanta and mixes bleed into bleedL/bleedR (each 128 floats,
    // overwritten). Always completes; never blocks.
    void consume(std::uint64_t tick, float* bleedL, float* bleedR, ConsumeOut& out) noexcept;
    bool removed() const noexcept;

    // ---- worker/control thread (atomics, safe to call concurrently with the audio thread) ----
    void setBleed(bool enabled, double dB) noexcept;
    bool setTopologyRow(const float* weights32) noexcept;  // false if non-finite; scaled at use so row sum <= g
    void clearTopology() noexcept;
    void setNetworkAvailable(bool available) noexcept;     // false: fade to local PSU, zero bleed
    void setEpoch(std::uint32_t epoch) noexcept { epoch_.store(epoch, std::memory_order_relaxed); }
    void setLatencyQuanta(unsigned q) noexcept { latency_.store(q > 48 ? 48 : q, std::memory_order_relaxed); }
    unsigned slot() const noexcept { return slot_; }
    Uuid128 id() const noexcept { return id_; }
    void keepAlive(std::shared_ptr<void> owner) { keepAlive_ = std::move(owner); }  // heap groups: pins the layout

private:
    friend class Group;
    friend class Registry;
    SharedLayout* L_ = nullptr;
    std::shared_ptr<void> keepAlive_;
    unsigned slot_ = 0;
    std::uint32_t gen_ = 0;
    Uuid128 id_;
    double fs_ = 48000;
    float decay_ = 0.f, rampStep_ = 0.f, duplicateGain_ = 0.7079458f;
    // control state
    std::atomic<std::uint32_t> epoch_{0}, latency_{kLiveLatencyQuanta};
    std::atomic<std::uint32_t> bleedGainBits_{0};  // linear g as float bits
    std::atomic<bool> available_{true}, topologyOn_{false};
    std::atomic<std::uint32_t> topology_[kMaxMembers];
    // audio-thread-only state (preallocated)
    bool removedLocal_ = false;
    float ownDemand_ = 0, ownPower_ = 0, coupling_ = 1.f;
    float heldDemand_[kMaxMembers] = {}, heldPower_[kMaxMembers] = {}, rampW_[kMaxMembers] = {};
    float scratch_[kAudioChannels][kQuantumFrames] = {};
};

// Worker-side group controller over a SharedLayout (heap or shared memory). Holds the reaper state.
class Group {
public:
    explicit Group(SharedLayout* layout);
    SharedLayout* layout() const noexcept { return L_; }
    JoinResult join(const Uuid128& memberId, double sampleRate, std::uint32_t railModelId, float nominalRailV,
                    std::unique_ptr<Member>& out, std::string* reason = nullptr);
    void leave(Member& m);
    // Removes members whose heartbeat has not advanced for timeoutNs (injected clock). Returns count.
    std::size_t reapStale(std::int64_t nowNs, std::int64_t timeoutNs = 500'000'000);
    unsigned memberCount() const noexcept;
    void bumpEpoch() noexcept;
    std::uint64_t rosterGeneration() const noexcept;
private:
    void forceRemove(unsigned slot);
    SharedLayout* L_;
    std::uint64_t lastBeat_[kMaxMembers] = {};
    std::int64_t lastChangeNs_[kMaxMembers] = {};
    std::uint32_t lastGen_[kMaxMembers] = {};
};

// In-process static registry ("broker"): owns heap groups keyed by (group UUID, session hash).
class Registry {
public:
    static Registry& instance();
    Registry() = default;
    JoinResult join(const GroupSpec& spec, const Uuid128& memberId, std::unique_ptr<Member>& out, std::string* reason = nullptr);
    void leave(Member& m);
    std::size_t reapStale(std::int64_t nowNs, std::int64_t timeoutNs = 500'000'000);
    std::size_t groupCount() const;
    std::size_t memberCount(const GroupSpec& spec) const;
    void clear();  // drops empty groups; members keep their layout alive via shared ownership
private:
    struct Entry {
        GroupSpec spec;
        std::shared_ptr<SharedLayout> layout;
        std::unique_ptr<Group> group;
    };
    Entry* find(const GroupSpec& spec) const;
    mutable std::mutex m_;  // worker-side only; never taken on the audio path
    std::vector<std::unique_ptr<Entry>> groups_;
};

}  // namespace cd::network
