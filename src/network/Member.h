// CircuitDrift network: member side of the instance network (CD-F11).
// Owner: cd_network.
//  * MemberClient — NON-AUDIO worker object: maps/validates a group region (hostile header
//    checks, group UUID + session-key hash), claims a member entry, polls the join result,
//    heartbeats while the transport is stopped, and leaves. It owns the MemberEndpoint.
//  * MemberEndpoint — AUDIO-THREAD object. process() is noexcept, allocation/lock/I/O-free and
//    bounded: per call O(frames) copies plus at most one ring publish and <= 64 ring reads per
//    quantum boundary. It never waits: a missing result frame for quantum q-L contributes zero
//    bleed and the shared-rail deviation decays (100 ms tau); stale frames are dropped by sample
//    index stamp; frames are copied privately and validated before use.
//    Network unavailable (broker lost: heartbeat word unchanged for 500 ms of processed audio or
//    broker Closed; evicted; corrupt ring; offline/prefetch render; Fs or epoch mismatch): influence
//    fades to the local PSU with zero bleed over 50 ms and the status says so. Broker-loss and
//    eviction latch; reconnection = a new MemberClient prepared off audio (G05 host matrix open).
// Tap point: the caller passes the member's audio after input trim and before its circuit and
// before received cross-talk (prevents recursive network feedback) and ADDS the returned bleed to
// the circuit input. Engine (cd_runtime) exposes no post-trim tap/insert, so an adapter using raw
// input is exact only at 0 dB input trim (integration remains Partial).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include "network/CrossTalk.h"
#include "network/Protocol.h"
#include "network/SharedRegion.h"

namespace cd::network {

namespace netstatus {
inline constexpr std::uint32_t kConnected = 1u << 0, kDegraded = 1u << 1, kUnavailable = 1u << 2,
                               kBrokerLost = 1u << 3, kEvicted = 1u << 4, kOffline = 1u << 5,
                               kRateMismatch = 1u << 6, kEpochMismatch = 1u << 7, kRingCorrupt = 1u << 8,
                               kSeek = 1u << 9, kUpOverflow = 1u << 10, kStaleDropped = 1u << 11,
                               kInvalidFrame = 1u << 12, kFading = 1u << 13, kUnbound = 1u << 14,
                               kNegativeTime = 1u << 15;
}

inline constexpr double kMaxBleedGain = 0.063095734448019325;   // -24 dB

struct NetContext {
    double sampleRate = 0;
    std::int64_t sampleStart = 0;      // absolute host timeline sample
    std::uint64_t epoch = 0;
    std::uint32_t frames = 0;
    bool offline = false;              // offline bounce / prefetch: realtime coupling unavailable
    double localRailV = 0;             // independently running local PSU (0 = nominal)
    double powerW = 0;                 // member supply power estimate for this call (synthetic)
};

struct NetResult {
    std::uint32_t status = 0;
    double railV = 0;                  // (1-w) * local + w * shared (end of call)
    double influence = 0;              // w in [0,1] (end of call)
};

class MemberEndpoint {
public:
    struct Diagnostics {
        std::int64_t published = 0, upOverflow = 0, resultsUsed = 0, resultsMissing = 0, staleDropped = 0,
                     invalidFrames = 0, seeks = 0, quanta = 0;
        bool brokerLost = false, evicted = false, ringCorrupt = false;
        double influence = 0, sharedRailV = 0;
    };
    // AUDIO THREAD. tap/bleedOut: `channels()` pointers of ctx.frames samples (bleedOut always
    // fully written; may not alias tap).
    NetResult process(const NetContext& ctx, const double* const* tap, double* const* bleedOut) noexcept;
    std::uint32_t channels() const noexcept { return channels_; }
    bool bound() const noexcept { return bound_.load(std::memory_order_acquire); }
    Diagnostics diagnostics() const noexcept { return diag_; }
    // Live cross-talk delay L*128/Fs seconds (display; not compensated host latency).
    double delaySeconds() const noexcept { return double(latencyQuanta_) * double(kQuantum) / hdr_.sampleRate; }
    std::uint32_t latencyQuanta() const noexcept { return latencyQuanta_; }

private:
    friend class MemberClient;
    void bind(std::byte* base, const RegionHeader& hdr, const Layout& l, std::uint32_t slot, const Uuid& member,
              std::uint64_t generation, std::uint32_t channels) noexcept;
    void onQuantum(std::int64_t q) noexcept;
    void fetchResult(std::int64_t tick) noexcept;
    void publish(std::int64_t q, double powerW, double localRailV) noexcept;
    bool downPayloadValid(const DownPayload& p, std::int64_t tick) const noexcept;

    std::atomic<bool> bound_{false};
    std::byte* base_ = nullptr;
    RegionHeader hdr_{};
    RegionHeader* shared_ = nullptr;
    MemberEntry* entry_ = nullptr;
    ShmRing up_, down_;
    Uuid member_{};
    std::uint64_t generation_ = 0;
    std::uint32_t channels_ = 1, latencyQuanta_ = kDefaultLatencyQuanta;
    std::uint64_t sequence_ = 0;
    // liveness
    std::uint64_t lastBrokerHb_ = 0;
    std::int64_t staleQuanta_ = 0, timeoutQuanta_ = 0;
    bool brokerLost_ = false, evicted_ = false, leaving_ = false;
    // timeline
    bool haveNext_ = false;
    std::int64_t expectedNext_ = 0, curQ_ = -1;
    // up quantum accumulation
    double upTap_[kMaxChannels][kQuantum] = {};
    std::uint32_t upFill_ = 0;
    bool upValid_ = false;
    double upEnergy_ = 0;
    // down result
    DownFrame scratch_{};
    UpFrame upScratch_{};
    double bleed_[kMaxChannels][kQuantum] = {};
    bool resValid_ = false;
    double railDev_ = 0, railDecay_ = 1;
    // influence fade
    double w_ = 0, wStep_ = 0;
    std::uint32_t callStatus_ = 0;
    Diagnostics diag_{};
};

struct JoinRequest {
    Uuid member{};                     // nil -> random
    Uuid expectedGroup{};              // required: group UUID from discovery
    std::string sessionKey;
    double sampleRate = 48000;
    std::uint32_t channels = 2;
    std::uint64_t timelineEpoch = 0;
    PsuManifest psu{};
};

class MemberClient {
public:
    enum class JoinState { Pending, Active, Rejected };
    // NON-AUDIO. Validates the region (hostile), group UUID and session key, then claims a free
    // entry and submits the request. nullptr + error on failure (e.g. group full, wrong key).
    static std::unique_ptr<MemberClient> attach(std::unique_ptr<SharedRegion> region, const JoinRequest& req,
                                                std::string& error);
    ~MemberClient();
    // NON-AUDIO: reads the entry; on first Active binds the endpoint (prepared before audio uses it).
    JoinState poll() noexcept;
    RejectReason reason() const noexcept { return reason_; }
    void heartbeat() noexcept;         // worker heartbeat (e.g. while transport is stopped)
    void leave() noexcept;             // influence ramps out at the broker, then the entry is freed
    MemberEndpoint& endpoint() noexcept { return *endpoint_; }
    std::uint32_t slot() const noexcept { return slot_; }
    const Uuid& member() const noexcept { return member_; }
    std::uint64_t generation() const noexcept { return generation_; }
private:
    MemberClient() = default;
    MemberEntry& entry() noexcept;
    std::unique_ptr<SharedRegion> region_;
    RegionHeader hdr_{};
    Layout layout_{};
    std::uint32_t slot_ = 0;
    Uuid member_{};
    std::uint32_t channels_ = 0;
    std::uint64_t generation_ = 0;
    JoinState state_ = JoinState::Pending;
    RejectReason reason_ = RejectReason::None;
    std::unique_ptr<MemberEndpoint> endpoint_;
};

}  // namespace cd::network
