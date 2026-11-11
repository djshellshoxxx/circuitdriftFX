// CircuitDrift network: member-side consumer (see NetworkMember.h).
#include "network/NetworkMember.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace cd::network {

NetworkMember::NetworkMember(const MemberConfig& cfg) : cfg_(cfg), local_(cfg.localPsu, cfg.sampleRate) {
    if (cfg.channels != 1 && cfg.channels != 2) throw std::invalid_argument("NetworkMember: channels must be 1|2");
    if (!(cfg.sampleRate > 0)) throw std::invalid_argument("NetworkMember: invalid sample rate");
    if (cfg.latencyQuanta < 1 || cfg.latencyQuanta >= kRingQuanta) throw std::invalid_argument("NetworkMember: latency");
    const double qps = cfg.sampleRate / double(kQuantumFrames);   // quanta per second
    decay_ = std::exp(-1.0 / (qps * kDemandDecayTauSeconds));
    rampStep_ = 1.0 / (kInfluenceRampSeconds * cfg.sampleRate);
    lostThreshold_ = static_cast<std::uint32_t>(std::ceil(qps * (kTickDeadlineSeconds + kDemandDecayTauSeconds)));
    frame_.channels = cfg.channels;
    frame_.localRailFraction = frame_.sharedRailFraction = frame_.railFraction = local_.railFraction();
}

bool NetworkMember::connect(MemberChannel* channel) {
    if (channel) {
        const ChannelIdentity& id = channel->identity();
        if (!(id.sampleRate == cfg_.sampleRate) || id.channels != cfg_.channels || id.member != cfg_.member)
            return false;
    }
    leaveDone_.store(false, std::memory_order_release);
    leaveRequested_.store(false, std::memory_order_release);
    pending_.store(channel, std::memory_order_release);
    return true;
}

void NetworkMember::disconnect() noexcept { pending_.store(nullptr, std::memory_order_release); }

bool NetworkMember::channelReleased(const MemberChannel* channel) const noexcept {
    return pending_.load(std::memory_order_acquire) != channel && observed_.load(std::memory_order_acquire) != channel;
}

bool NetworkMember::fetchExact(std::uint64_t want) noexcept {
    auto& ring = active_->downlink();
    const std::size_t n = ring.acquire();   // one acquire per quantum
    FrameExpectation x;
    x.type = PayloadType::Result;
    x.group = id_.group;
    x.session = id_.session;
    x.member = cfg_.member;
    x.sampleRate = cfg_.sampleRate;
    x.epoch = epoch_;
    x.minGeneration = id_.joinGeneration;
    std::size_t used = 0;
    bool found = false;
    for (; used < n; ++used) {
        const ResultFrame& f = ring.at(used);
        FrameError e = validate(f.header, x);
        if (e == FrameError::None && f.header.channels != cfg_.channels) e = FrameError::InvalidChannels;
        if (e != FrameError::None) { ++counters_.rejected[static_cast<std::size_t>(e)]; continue; }
        const std::uint64_t t = f.header.sampleStart / kQuantumFrames;
        if (t < want) { ++counters_.staleDiscarded; continue; }
        if (t > want) break;                 // future result: leave it queued
        frame_.sourceTick = t;
        frame_.sharedRailFraction = std::isfinite(f.payload.railFraction) ? f.payload.railFraction : local_.railFraction();
        for (std::uint32_t c = 0; c < cfg_.channels; ++c)
            for (std::uint32_t s = 0; s < kQuantumFrames; ++s) {
                const double v = f.payload.bleed[c][s];
                frame_.bleed[c][s] = std::isfinite(v) ? v : 0.0;
            }
        if (f.payload.flags & kResultDegraded) frame_.status |= kNetDegraded;
        found = true;
        ++used;
        break;
    }
    ring.release(used);
    return found;
}

void NetworkMember::beginQuantum(bool realtime) noexcept {
    MemberChannel* p = pending_.load(std::memory_order_acquire);
    if (p != active_) {
        active_ = p;
        if (p) id_ = p->identity();   // trivially copyable; written before the release store
        misses_ = 0;
        lost_ = false;
        observed_.store(p, std::memory_order_release);
    }
    frame_.status = 0;
    frame_.epoch = epoch_;
    frame_.tick = tick_;
    frame_.localRailFraction = local_.railFraction();
    bool exact = false;
    bool brokerGone = false, removed = false;
    if (active_) {
        frame_.status |= kNetConnected;
        ChannelControl& c = active_->control();
        brokerGone = c.brokerState.load(std::memory_order_acquire) == static_cast<std::uint32_t>(BrokerState::Gone);
        removed = c.membership.load(std::memory_order_acquire) == static_cast<std::uint32_t>(MembershipState::Removed);
        if (realtime && tick_ >= cfg_.latencyQuanta) exact = fetchExact(tick_ - cfg_.latencyQuanta);
    }
    if (exact) {
        ++counters_.exact;
        frame_.status |= kNetExact;
        misses_ = 0;
        lost_ = false;
        excess_ = frame_.sharedRailFraction - frame_.localRailFraction;
    } else {
        ++counters_.decayed;
        frame_.status |= kNetDecayed;
        std::memset(frame_.bleed, 0, sizeof(frame_.bleed));
        excess_ *= decay_;
        frame_.sharedRailFraction = frame_.localRailFraction + excess_;
        if (active_ && ++misses_ > lostThreshold_ && !lost_) { lost_ = true; ++counters_.brokerLossEvents; }
    }
    const bool leaving = leaveRequested_.load(std::memory_order_acquire);
    if (brokerGone || removed) {
        if (!lost_) ++counters_.brokerLossEvents;
        lost_ = true;
    }
    if (lost_) frame_.status |= kNetBrokerLost;
    if (removed) frame_.status |= kNetRemoved;
    if (leaving) frame_.status |= kNetLeaving;
    if (!realtime) frame_.status |= kNetUnavailable;
    // Ramp in once exact results flow; transient misses keep the target (decay rule applies).
    target_ = (active_ && realtime && !lost_ && !leaving && (exact || influence_ > 0)) ? 1.0 : 0.0;
    frame_.influence = influence_;
    frame_.railFraction = frame_.localRailFraction + influence_ * (frame_.sharedRailFraction - frame_.localRailFraction);
}

void NetworkMember::endQuantum(const MemberDemand& d, bool realtime) noexcept {
    auto clean = [](double v) { return std::isfinite(v) ? v : 0.0; };
    const double current = clean(d.currentA) > 0 ? clean(d.currentA) : 0.0;
    if (active_ && realtime) {   // non-realtime (prefetch/offline) never feeds the live group
        DemandFrame* f = active_->uplink().beginWrite();
        if (!f) {
            ++counters_.uplinkDrops;
        } else {
            stampHeader<DemandPayload>(f->header);
            f->header.group = id_.group;
            f->header.session = id_.session;
            f->header.member = cfg_.member;
            f->header.generation = id_.joinGeneration;
            f->header.sampleRate = cfg_.sampleRate;
            f->header.channels = cfg_.channels;
            f->header.epoch = epoch_;
            f->header.sampleStart = tick_ * kQuantumFrames;
            f->header.sequence = sequence_++;
            f->payload.currentA = current;
            f->payload.powerW = clean(d.powerW);
            f->payload.activity = clean(d.activity);
            f->payload.railModelId = id_.railModelId;
            f->payload.frames = kQuantumFrames;
            f->payload.flags = 0;
            std::memcpy(f->payload.tap, tap_, sizeof(tap_));
            active_->uplink().commit();
        }
    }
    (void)local_.stepQuantum(current);
    ++counters_.quanta;
    ++tick_;
    pos_ = 0;
    std::memset(tap_, 0, sizeof(tap_));
}

void NetworkMember::process(const MemberBlock& b) noexcept {
    const std::uint32_t ch = cfg_.channels;
    const bool jump = started_ && b.epoch == epoch_ && b.timelineSample != tick_ * kQuantumFrames + pos_;
    if (!started_ || b.epoch != epoch_ || jump) {
        if (jump) ++counters_.timelineJumps;
        else if (started_) ++counters_.epochChanges;
        started_ = true;
        epoch_ = b.epoch;
        tick_ = b.timelineSample / kQuantumFrames;
        pos_ = static_cast<std::uint32_t>(b.timelineSample % kQuantumFrames);
        std::memset(tap_, 0, sizeof(tap_));
        excess_ = 0;   // shared estimate is not carried across a seek
        beginQuantum(b.realtime);
    }
    if (active_) active_->control().memberHeartbeat.fetch_add(1, std::memory_order_release);
    std::uint32_t done = 0;
    while (done < b.frames) {
        const std::uint32_t n = std::min(b.frames - done, kQuantumFrames - pos_);
        for (std::uint32_t c = 0; c < ch; ++c) {
            const double* in = nullptr;   // mono host input feeds both channels of a stereo member
            if (b.tap && b.channels > 0) in = c < b.channels ? b.tap[c] : b.tap[0];
            for (std::uint32_t s = 0; s < n; ++s) {
                const double v = in ? in[done + s] : 0.0;
                tap_[c][pos_ + s] = std::isfinite(v) ? v : 0.0;
            }
        }
        double inf = influence_;
        for (std::uint32_t s = 0; s < n; ++s) {
            inf = inf < target_ ? std::min(target_, inf + rampStep_) : std::max(target_, inf - rampStep_);
            if (b.bleedOut)
                for (std::uint32_t c = 0; c < ch; ++c)
                    if (b.bleedOut[c]) b.bleedOut[c][done + s] = inf * frame_.bleed[c][pos_ + s];
        }
        influence_ = inf;
        if (influence_ == 0.0 && leaveRequested_.load(std::memory_order_relaxed))
            leaveDone_.store(true, std::memory_order_release);
        pos_ += n;
        done += n;
        if (pos_ == kQuantumFrames) {
            endQuantum(b.demand, b.realtime);
            beginQuantum(b.realtime);
        }
    }
}

}  // namespace cd::network
