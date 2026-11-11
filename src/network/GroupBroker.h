// CircuitDrift network: group broker core (P15, CD-F11/T11). NON-AUDIO worker logic.
// Owner: cd_network. Spec 04-instance-network (D17). Deterministic for a given clock and call
// order: the clock is injected (BrokerClock), members are kept in fixed member-UUID order.
// The real per-user broker PROCESS (discovery, OS shared memory, per-user permissions, sandbox
// handling, crash/restart supervision) is a platform task: gate G05 stays open. Here the broker
// is an in-process object; "broker death" is modelled by no longer pumping it (crash) or by
// destroying it (clean exit: channels are marked BrokerState::Gone). Members keep their channel
// memory alive through shared_ptr, as a mapping would.
//
// Groups: random 128-bit group UUID + human name + explicit session key (+ an ephemeral session
// UUID carried in every frame). <= kMaxGroupsPerBroker groups, <= kMaxMembersPerGroup members.
// Join is opt-in (a member calls join) and checked per member: session key, same PSU manifest id,
// rail model, nominal rail and sample rate as the group, channels 1|2. Rejections carry a reason.
// Ticks: for group tick k the broker advances when every active member's frame for k is present,
// or when 50 ms (realtime clock) have passed since the earliest pending frame for a tick >= k
// arrived. Missing members then contribute zero audio and their last demand decayed with a
// 100 ms time constant per missed tick; the tick is flagged degraded. A member counts as active
// for tick k once it delivered a frame with tick <= k in the current epoch. Frames older than the
// next tick are late and dropped. The group epoch is the newest epoch seen; older-epoch frames
// are rejected (stale after seek/loop) and an epoch change restarts tick alignment.
// Each tick runs one shared ReservoirPsu step on the sum of contribution-weighted demands and
// mixes bleed from the pre-circuit taps (Bleed.h), then writes one Result frame per member.
// Join/leave contribution ramps over 50 ms of ticks. No heartbeat (callback counter or frames)
// for 500 ms removes a member (membership word -> Removed) and publishes a new roster
// generation. A full downlink ring drops that member's result (counted); the broker never waits.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include "network/Bleed.h"
#include "network/PreparedRing.h"
#include "network/Protocol.h"
#include "network/SharedPsu.h"

namespace cd::network {

class BrokerClock {
public:
    virtual ~BrokerClock() = default;
    virtual std::int64_t nowMicros() noexcept = 0;
};
class ManualClock final : public BrokerClock {
public:
    std::int64_t nowMicros() noexcept override { return t; }
    void advanceMicros(std::int64_t us) noexcept { t += us; }
    std::int64_t t = 0;
};
class SteadyBrokerClock final : public BrokerClock {
public:
    std::int64_t nowMicros() noexcept override;
};

struct GroupSpec {
    std::string name;
    std::string sessionKey;
    double sampleRate = 48000;
    PsuSpec psu;
    BleedConfig bleed;
};

struct MemberDescriptor {
    Uuid128 member;
    std::uint64_t psuManifestId = 0;
    std::uint64_t railModelId = 0;
    double nominalRailV = 0;
    double sampleRate = 0;
    std::uint32_t channels = 2;
    bool bleedOptIn = false;     // audio bleed is opted in separately from shared demand
};

enum class GroupError : std::uint32_t {
    None = 0, TooManyGroups, EmptyName, EmptySessionKey, InvalidSampleRate, InvalidPsu, InvalidBleed
};
enum class JoinReject : std::uint32_t {
    None = 0, NoSuchGroup, SessionKeyMismatch, GroupFull, NilMember, DuplicateMember, SampleRateMismatch,
    PsuManifestMismatch, RailModelMismatch, NominalRailMismatch, InvalidChannels, AllocationFailed
};
const char* toString(GroupError e) noexcept;
const char* toString(JoinReject e) noexcept;

struct CreateGroupResult {
    GroupError error = GroupError::None;
    std::string reason;
    Uuid128 group;
    Uuid128 session;
};

struct JoinResult {
    JoinReject reject = JoinReject::None;
    std::string reason;                       // per-member human-readable reason
    std::shared_ptr<MemberChannel> channel;   // null when rejected
    std::uint64_t generation = 0;
    bool ok() const noexcept { return reject == JoinReject::None; }
};

struct MemberTickInfo {
    Uuid128 member;
    bool participating = false;   // active for this tick
    bool present = false;         // exact frame arrived
    double demandA = 0;           // effective (possibly decayed) demand before contribution
    double contribution = 0;      // ramp 0..1
};
struct TickRecord {
    std::uint64_t epoch = 0;
    std::uint64_t tick = 0;
    bool degraded = false;
    std::uint32_t active = 0;
    std::uint32_t present = 0;
    double totalDemandA = 0;
    double railVolts = 0;
    double railFraction = 0;
    std::vector<MemberTickInfo> members;
};

struct GroupStatus {
    bool exists = false;
    std::string name;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::uint64_t nextTick = 0;
    std::uint64_t ticksAdvanced = 0;
    std::uint64_t degradedTicks = 0;
    std::uint64_t lateFrames = 0;
    std::uint64_t rejectedFrames = 0;
    std::uint64_t downlinkDrops = 0;
    std::uint64_t removedByTimeout = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(FrameError::Count)> rejectedByReason{};
    std::vector<Uuid128> roster;
    TickRecord lastTick;
};

class GroupBroker {
public:
    GroupBroker(RingAllocator& alloc, BrokerClock& clock, std::uint64_t uuidSeed);
    ~GroupBroker();   // clean exit: every channel's brokerState -> Gone
    GroupBroker(const GroupBroker&) = delete;
    GroupBroker& operator=(const GroupBroker&) = delete;

    CreateGroupResult createGroup(const GroupSpec& spec);
    JoinResult join(const Uuid128& group, std::string_view sessionKey, const MemberDescriptor& d);
    // Opt-out: contribution ramps out over 50 ms of ticks, then the member is removed.
    bool leave(const Uuid128& group, const Uuid128& member);
    // Worker step: heartbeats, drain uplinks, advance ready/expired ticks (<= 64 per group per
    // call), heartbeat timeouts. Returns the number of ticks advanced across all groups.
    std::uint32_t pump();
    GroupStatus status(const Uuid128& group) const;
    std::size_t groupCount() const noexcept { return groups_.size(); }

private:
    struct Pending {
        bool valid = false;
        std::uint64_t tick = 0;
        std::int64_t arrivalUs = 0;
        DemandPayload payload;
    };
    struct Member {
        MemberDescriptor desc;
        std::shared_ptr<MemberChannel> channel;
        std::uint64_t joinGeneration = 0;
        std::int64_t lastSeenUs = 0;
        std::uint64_t lastHeartbeat = 0;
        bool started = false;
        std::uint64_t firstTick = 0;
        double lastDemandA = 0;
        double contribution = 0;
        bool leaving = false;
        std::int64_t leaveUs = 0;
        std::vector<Pending> pending;   // kRingQuanta, indexed by tick % kRingQuanta
    };
    struct Group {
        GroupSpec spec;
        Uuid128 id;
        Uuid128 session;
        std::array<std::uint8_t, 32> keyDigest{};
        std::uint64_t generation = 1;
        std::uint64_t epoch = 0;
        bool epochSeen = false;
        bool tickValid = false;
        std::uint64_t nextTick = 0;
        ReservoirPsu psu;
        std::vector<std::unique_ptr<Member>> members;   // sorted by member UUID
        std::vector<double> weights;
        std::vector<BleedSource> sources;
        std::vector<BleedSink> sinks;
        std::vector<ResultFrame*> slots;
        ResultFrame scratch;                            // sink for a full downlink
        GroupStatus stats;
    };
    Group* find(const Uuid128& id) noexcept;
    const Group* find(const Uuid128& id) const noexcept;
    void rosterChanged(Group& g);
    void drain(Group& g, std::int64_t now);
    bool advanceOne(Group& g, std::int64_t now);
    void expire(Group& g, std::int64_t now);
    void adoptEpoch(Group& g, std::uint64_t epoch);

    RingAllocator& alloc_;
    BrokerClock& clock_;
    std::mt19937_64 rng_;
    std::vector<std::unique_ptr<Group>> groups_;
};

}  // namespace cd::network
