// CircuitDrift network: member-side audio consumer/producer (P15, CD-F11/T11). AUDIO-SAFE.
// Owner: cd_network. Not wired into cd::runtime::Engine yet: a future Engine hook calls
// process() per host block and consumes frame() as the "available network frame" of the
// per-sample order (input trim -> [network tap] -> circuit -> + received bleed ...).
//
// Quantum grid: 128-frame quanta aligned to the absolute timeline (tick k covers timeline samples
// [128k, 128k+128)); the grid restarts at every new epoch from MemberBlock::timelineSample (a
// partial first quantum is zero-padded at its start). Within an epoch the member counts processed
// frames; a block whose timelineSample does not continue that count (host skipped callbacks)
// re-grids like a seek (counted as a timeline jump). Host block partition does not change the
// published frames or the bleed stream.
// Per quantum k (all bounded, no waits): at the quantum start the member performs ONE acquire on
// its downlink, discards invalid (validate(): stale epoch/generation, wrong group/session/Fs...)
// and older frames, and takes only the exact completed result for tick k-L (L = 8). A future
// frame stays queued. No exact frame -> local rule: zero bleed, and the shared-rail excess over
// the local rail decays with the 100 ms demand time constant (counted as decayed). When no exact
// frame arrived for ceil((50 ms deadline + 100 ms tau) * Fs / 128) consecutive quanta (after the
// L buffered results ran out: ~173 ms at 48 kHz), or the broker marked itself Gone, or the member
// was Removed, the broker is treated as lost. Note: a deadline-forced (degraded) tick arrives at
// least 50 ms late, i.e. after the L = 1024-sample slack, so members that kept up see it as
// missing (decay rule) rather than consuming the degraded result. At the quantum end the member publishes
// its demand and pre-circuit tap for tick k (a full uplink drops the frame, counted) and steps its
// independently running local PSU on its own demand.
// Influence (0..1) ramps linearly over 50 ms per sample toward 1 while connected, realtime and
// receiving exact frames, toward 0 on broker loss/removal/leave/non-realtime. Bleed output =
// influence * bleed; rail = local + influence * (shared - local). Audio never stops.
// Non-realtime blocks (host prefetch/offline) publish Network unavailable and fade to local PSU
// with zero bleed; deterministic offline coupling comes only from CoRender replay.
// Labels: kLiveCouplingLabel ("approximate live coupling"); delay liveDisplayDelaySeconds(Fs).
// Threading: constructor/connect/disconnect/requestLeave are non-audio; process(), frame() and
// the accessors are noexcept and allocation/lock/syscall free. connect() hands the channel over
// through an atomic pointer picked up at the next quantum start; channelReleased(p) tells the
// owner when the audio thread no longer references p (keep the shared_ptr alive until then).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include "network/PreparedRing.h"
#include "network/Protocol.h"
#include "network/SharedPsu.h"

namespace cd::network {

enum NetworkStatus : std::uint32_t {
    kNetConnected = 1u << 0,    // a channel is attached
    kNetExact = 1u << 1,        // this quantum uses the exact broker result for k-L
    kNetDecayed = 1u << 2,      // no exact frame: local decay/zero rule
    kNetDegraded = 1u << 3,     // broker advanced the source tick with missing members
    kNetBrokerLost = 1u << 4,   // broker gone/silent: fading to local PSU
    kNetUnavailable = 1u << 5,  // non-realtime (prefetch/offline) or Fs/epoch invalid
    kNetRemoved = 1u << 6,      // broker removed this member (heartbeat timeout)
    kNetLeaving = 1u << 7,      // leave requested, influence ramping out
};

struct MemberConfig {
    Uuid128 member;
    double sampleRate = 48000;
    std::uint32_t channels = 2;
    PsuSpec localPsu;
    std::uint32_t latencyQuanta = kLiveLatencyQuanta;
};

struct MemberDemand {
    double currentA = 0;
    double powerW = 0;
    double activity = 0;
};

struct MemberBlock {
    const double* const* tap = nullptr;   // PRE-CIRCUIT input (after trim), channels x frames
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;
    std::uint64_t epoch = 0;
    std::uint64_t timelineSample = 0;     // absolute timeline position of the block start
    bool realtime = true;
    MemberDemand demand;
    double* const* bleedOut = nullptr;    // optional: influence-scaled bleed, channels x frames
};

// The per-quantum network frame a future Engine hook consumes.
struct NetworkFrame {
    std::uint64_t epoch = 0;
    std::uint64_t tick = 0;            // member quantum this frame applies to
    std::uint64_t sourceTick = 0;      // broker tick it came from (tick - L) when exact
    std::uint32_t status = 0;          // NetworkStatus bits
    std::uint32_t channels = 0;
    std::uint32_t frames = kQuantumFrames;
    double influence = 0;              // at the quantum start
    double sharedRailFraction = 1;     // broker readout (or decayed estimate)
    double localRailFraction = 1;      // own local PSU
    double railFraction = 1;           // local + influence * (shared - local)
    double bleed[kMaxChannels][kQuantumFrames] = {};   // raw broker bleed (before influence)
};

struct MemberCounters {
    std::uint64_t quanta = 0;
    std::uint64_t exact = 0;
    std::uint64_t decayed = 0;
    std::uint64_t staleDiscarded = 0;
    std::uint64_t uplinkDrops = 0;
    std::uint64_t epochChanges = 0;
    std::uint64_t timelineJumps = 0;   // discontinuity within an epoch (dropped callbacks): re-grid
    std::uint64_t brokerLossEvents = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(FrameError::Count)> rejected{};
};

class NetworkMember {
public:
    explicit NetworkMember(const MemberConfig& cfg);
    NetworkMember(const NetworkMember&) = delete;
    NetworkMember& operator=(const NetworkMember&) = delete;

    // ---- non-audio ----
    bool connect(MemberChannel* channel);   // false if Fs/channels/member identity mismatch
    void disconnect() noexcept;
    bool channelReleased(const MemberChannel* channel) const noexcept;
    void requestLeave() noexcept { leaveRequested_.store(true, std::memory_order_release); }
    bool leaveComplete() const noexcept { return leaveDone_.load(std::memory_order_acquire); }

    // ---- audio ----
    void process(const MemberBlock& block) noexcept;
    const NetworkFrame& frame() const noexcept { return frame_; }
    double influence() const noexcept { return influence_; }
    std::uint32_t status() const noexcept { return frame_.status; }
    const MemberCounters& counters() const noexcept { return counters_; }
    const MemberConfig& config() const noexcept { return cfg_; }
    std::uint32_t lostThresholdQuanta() const noexcept { return lostThreshold_; }

private:
    void beginQuantum(bool realtime) noexcept;
    void endQuantum(const MemberDemand& d, bool realtime) noexcept;
    bool fetchExact(std::uint64_t want) noexcept;

    MemberConfig cfg_;
    ReservoirPsu local_;
    std::atomic<MemberChannel*> pending_{nullptr};
    std::atomic<const MemberChannel*> observed_{nullptr};
    std::atomic<bool> leaveRequested_{false};
    std::atomic<bool> leaveDone_{false};
    MemberChannel* active_ = nullptr;
    ChannelIdentity id_{};
    bool started_ = false;
    std::uint64_t epoch_ = 0;
    std::uint64_t tick_ = 0;
    std::uint32_t pos_ = 0;            // frame position within the current quantum
    std::uint64_t sequence_ = 0;
    std::uint32_t misses_ = 0;
    std::uint32_t lostThreshold_ = 0;
    bool lost_ = false;
    double excess_ = 0;                // shared - local rail at the last exact frame (decaying)
    double decay_ = 1;
    double rampStep_ = 0;
    double influence_ = 0;
    double target_ = 0;
    double tap_[kMaxChannels][kQuantumFrames] = {};
    NetworkFrame frame_;
    MemberCounters counters_;
};

}  // namespace cd::network
