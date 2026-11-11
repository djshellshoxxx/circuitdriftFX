// CircuitDrift network: per-user local broker and group broker (CD-F11 membership + tick engine).
// Owner: cd_network. Runs on a broker WORKER thread (or in a separate broker process over POSIX
// shared memory); never on an audio thread. create() allocates; service()/close() are noexcept,
// allocation-free and bounded (<= 64 frames drained per member, <= 64 ticks per call).
//
// Membership (all through the shared region's member table, no sockets):
//   member: CAS entry Free->Claimed, write request, release-store Requested
//   broker: validate compatibility (Fs, channels, rail model, nominal rail, PSU manifest hash,
//           timeline epoch, duplicate member UUID) -> Active (new entry generation, roster
//           generation +1) or Rejected with a per-member reason
//   lease:  member heartbeat word must change at least every 500 ms (broker clock) or the member is
//           evicted (Rejected/Evicted, roster generation +1). Corrupt rings also evict.
//   leave:  member stores Leaving; its influence ramps out over 50 ms, then the entry is freed.
// Ticks: tick k is computed when every active member's frame for k is present, or 50 ms after the
// first frame for k arrived (missing members: zero new audio, last demand decayed with 100 ms tau,
// tick flagged degraded). One shared reservoir solve per tick (synthetic PSU fixture), bleed per
// receiver (CrossTalk.h), results pushed to each member's down ring. Join/leave ramps 50 ms.
// Timing note: tick k results are consumed by members at quantum k+L (L = 8 by default); this is a
// sonic cross-talk delay (1024/Fs s), not compensated host latency. Live coupling is approximate
// and NOT sample-perfect under arbitrary DAW scheduling (D17); see CoRender.h for exact replay.
// Session isolation: one region per group, named from a random session token; discovery returns a
// group's region only for its exact session key; the region header stores the key hash and group
// UUID which members verify. A LocalBroker holds at most 8 groups.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "network/CrossTalk.h"
#include "network/Protocol.h"
#include "network/SharedRegion.h"

namespace cd::network {

struct GroupConfig {
    Uuid group{};                  // nil -> random 128-bit
    Uuid session{};                // ephemeral session token; nil -> random
    std::string name = "group";    // <= 63 bytes
    std::string sessionKey;        // explicit user key (non-empty)
    double sampleRate = 48000;
    std::uint32_t channels = 2;    // group bus layout (members may be mono or stereo)
    std::uint64_t timelineEpoch = 0;
    std::uint32_t maxMembers = kMaxMembers;   // 2..32
    std::uint32_t latencyQuanta = kDefaultLatencyQuanta;
    PsuManifest psu{};
    bool sharedDemand = true;
    BleedConfig bleed{};
    std::uint64_t bleedSeed = 1;
    bool useTopology = false;      // optional matrix by entry slot; enforced to row sum <= g
    std::vector<std::vector<double>> topology;   // [receiver][source], up to maxMembers^2
    double deadlineMs = kDeadlineMs;
    double leaseMs = kLeaseMs;
};

class GroupBroker {
public:
    struct Diagnostics {
        std::int64_t ticks = 0, degradedTicks = 0, framesIn = 0, staleFrames = 0, invalidFrames = 0,
                     aheadFrames = 0, resyncs = 0, resultsOut = 0, resultsDropped = 0, joins = 0, rejects = 0,
                     evictions = 0, leaves = 0, sanitizedEntries = 0, services = 0;
        std::uint32_t active = 0;
        std::uint64_t rosterGeneration = 0;
        std::int64_t nextTick = -1;
        double railV = 0;
        bool rowClamped = false;
        double maxRowAbsSum = 0;   // max over receivers of the effective absolute row sum (last tick)
        double gain = 0;
    };

    // Initializes `region` (>= layout bytes) as a fresh CDIP group region. Non-audio.
    static std::unique_ptr<GroupBroker> create(const GroupConfig& cfg, std::unique_ptr<SharedRegion> region,
                                               std::string& error);
    // Broker worker. nowMs: monotonic broker clock in milliseconds.
    void service(double nowMs) noexcept;
    // Graceful shutdown: brokerState = Closed (members fade to local immediately).
    void close() noexcept;

    Diagnostics diagnostics() const noexcept { return diag_; }
    const GroupConfig& config() const noexcept { return cfg_; }
    const std::string& regionName() const noexcept { return region_->name(); }
    SharedRegion& region() noexcept { return *region_; }
    double gain() const noexcept { return gain_; }
    // Effective (enforced) topology row sum for slot i (default topology: g when N > 1).
    double effectiveRowAbsSum(std::uint32_t slot) const noexcept;

private:
    struct Pending { std::int64_t tick = -1; UpPayload p; };
    struct Slot {
        bool active = false, leaving = false;
        std::uint64_t generation = 0;
        Uuid member{};
        std::uint32_t channels = 0;
        ShmRing up, down;
        double w = 0, wTarget = 0;
        double lastDemand = 0;
        std::uint64_t lastHeartbeat = 0;
        double lastHeartbeatMs = 0, stateSinceMs = 0;
        std::uint64_t seenState = ~std::uint64_t(0);
        double sign = 1;
        std::uint64_t sequence = 0;
        std::vector<Pending> pending;   // kRingSlots entries, preallocated
    };
    GroupBroker() = default;
    void scanEntries(double nowMs) noexcept;
    void activate(std::uint32_t s, const MemberEntry& req, double nowMs) noexcept;
    void evict(std::uint32_t s, RejectReason why) noexcept;
    void freeSlot(std::uint32_t s) noexcept;
    RejectReason checkRequest(std::uint32_t s, const MemberEntry& req) const noexcept;
    void drain(std::uint32_t s, double nowMs) noexcept;
    bool upPayloadValid(const UpPayload& p, std::uint32_t channels) const noexcept;
    void advanceTicks(double nowMs) noexcept;
    void computeTick(std::int64_t k) noexcept;
    MemberEntry& entry(std::uint32_t s) noexcept;
    RegionHeader& sharedHeader() noexcept { return *reinterpret_cast<RegionHeader*>(region_->data()); }
    std::uint32_t rosterSize() const noexcept;

    GroupConfig cfg_{};
    std::unique_ptr<SharedRegion> region_;
    RegionHeader hdr_{};             // private copy of the immutable header
    Layout layout_{};
    SharedReservoir psu_{};
    double gain_ = 0;
    bool topo_ = false;
    TopologyMatrix matrix_{};
    double rampStep_ = 0, demandDecay_ = 0;
    std::vector<Slot> slots_;
    std::vector<double> weights_;    // [slot][kQuantum] per-sample ramp weights of the current tick
    std::vector<double> bleed_;      // [kMaxChannels][kQuantum]
    struct FirstSeen { std::int64_t tick = -1; double ms = 0; };
    FirstSeen firstSeen_[kRingSlots];
    std::int64_t nextTick_ = -1;
    std::int64_t resyncTick_ = -1;   // discontinuous frame tick observed this service
    Diagnostics diag_{};
};

// Factory for group regions: (name, bytes, error) -> region (arena or POSIX shm).
using RegionFactory = std::function<std::unique_ptr<SharedRegion>(const std::string&, std::size_t, std::string&)>;

// Per-user broker for up to 8 groups. Non-audio (worker thread).
class LocalBroker {
public:
    explicit LocalBroker(RegionFactory factory) : factory_(std::move(factory)) {}
    // Creates a group (random group UUID / session token when nil). Rejects a 9th group, an empty
    // session key or an over-long name.
    GroupBroker* createGroup(GroupConfig cfg, std::string& error);
    bool closeGroup(const Uuid& group) noexcept;
    // Discovery: the group's region name only for the exact session key; otherwise empty.
    std::string discover(const Uuid& group, std::string_view sessionKey) const;
    void service(double nowMs) noexcept;
    std::size_t groupCount() const noexcept { return groups_.size(); }
private:
    RegionFactory factory_;
    std::vector<std::unique_ptr<GroupBroker>> groups_;
};

}  // namespace cd::network
