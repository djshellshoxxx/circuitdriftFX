// CircuitDrift network: member client/endpoint (see Member.h). Owner: cd_network.
// Threading: MemberClient non-audio; MemberEndpoint::process audio thread (noexcept, no alloc,
// no locks, no I/O, bounded).
#include "network/Member.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cd::network {

// ---------------------------------------------------------------------------------------------
// MemberClient (non-audio)

std::unique_ptr<MemberClient> MemberClient::attach(std::unique_ptr<SharedRegion> region, const JoinRequest& req,
                                                   std::string& error) {
    if (!region) { error = "no region"; return nullptr; }
    Layout l;
    const RegionError re = validateRegion(region->data(), region->size(), l);
    if (re != RegionError::None) { error = std::string("region rejected: ") + regionErrorName(re); return nullptr; }
    RegionHeader h;
    std::memcpy(&h, region->data(), sizeof h);
    // The private copy is what we use from now on; it must still be the header that validated.
    if (hash64(&h, offsetof(RegionHeader, immutableHash)) != h.immutableHash || h.totalBytes != l.totalBytes ||
        h.maxMembers != l.maxMembers || h.latencyQuanta < 1 || h.latencyQuanta > kRingSlots / 2) {
        error = "region header changed during attach"; return nullptr;
    }
    if (std::memcmp(h.group, req.expectedGroup.b.data(), 16) != 0) { error = regionErrorName(RegionError::Group); return nullptr; }
    Uuid session;
    std::memcpy(session.b.data(), h.session, 16);
    if (sessionKeyHash(session, req.sessionKey) != h.sessionKeyHash) { error = regionErrorName(RegionError::SessionKey); return nullptr; }
    if (req.channels < 1 || req.channels > kMaxChannels) { error = "channels must be 1 or 2"; return nullptr; }
    if (loadAcquire(reinterpret_cast<RegionHeader*>(region->data())->brokerState) == std::uint64_t(BrokerState::Closed)) {
        error = rejectReasonName(RejectReason::BrokerClosed); return nullptr;
    }

    std::unique_ptr<MemberClient> c(new MemberClient());
    c->region_ = std::move(region);
    c->hdr_ = h;
    c->layout_ = l;
    c->member_ = req.member.isNil() ? Uuid::random() : req.member;
    c->channels_ = req.channels;
    c->endpoint_ = std::make_unique<MemberEndpoint>();
    std::byte* base = c->region_->data();
    bool claimed = false;
    for (std::uint32_t s = 0; s < l.maxMembers && !claimed; ++s) {
        auto& e = *reinterpret_cast<MemberEntry*>(base + l.entryOffset + std::uint64_t(s) * l.entryStride);
        if (!compareExchange(e.state, std::uint64_t(EntryState::Free), std::uint64_t(EntryState::Claimed))) continue;
        std::memcpy(e.member, c->member_.b.data(), 16);
        e.sampleRate = req.sampleRate;
        e.timelineEpoch = req.timelineEpoch;
        e.railModelId = req.psu.railModelId;
        e.nominalRailV = req.psu.nominalV;
        e.psuManifestHash = req.psu.hash();
        e.channels = req.channels;
        storeRelease(e.reason, 0);
        storeRelease(e.state, std::uint64_t(EntryState::Requested));
        c->slot_ = s;
        claimed = true;
    }
    if (!claimed) { error = rejectReasonName(RejectReason::Full); return nullptr; }
    return c;
}

MemberClient::~MemberClient() {
    if (!region_) return;
    // Release the entry if it is still ours (Active/Leaving -> Leaving so the broker ramps out;
    // Rejected/Requested -> Free).
    MemberEntry& e = entry();
    const std::uint64_t st = loadAcquire(e.state);
    if (state_ == JoinState::Active && st == std::uint64_t(EntryState::Active) && loadAcquire(e.generation) == generation_)
        compareExchange(e.state, st, std::uint64_t(EntryState::Leaving));
    else if (st == std::uint64_t(EntryState::Rejected) || st == std::uint64_t(EntryState::Requested))
        compareExchange(e.state, st, std::uint64_t(EntryState::Free));
}

MemberEntry& MemberClient::entry() noexcept {
    return *reinterpret_cast<MemberEntry*>(region_->data() + layout_.entryOffset + std::uint64_t(slot_) * layout_.entryStride);
}

MemberClient::JoinState MemberClient::poll() noexcept {
    if (state_ != JoinState::Pending) return state_;
    MemberEntry& e = entry();
    const std::uint64_t st = loadAcquire(e.state);
    if (st == std::uint64_t(EntryState::Active)) {
        generation_ = loadAcquire(e.generation);
        endpoint_->bind(region_->data(), hdr_, layout_, slot_, member_, generation_, channels_);
        state_ = JoinState::Active;
    } else if (st == std::uint64_t(EntryState::Rejected)) {
        const std::uint64_t r = loadAcquire(e.reason);
        reason_ = r <= std::uint64_t(RejectReason::Full) ? RejectReason(r) : RejectReason::BadRequest;
        state_ = JoinState::Rejected;
        compareExchange(e.state, st, std::uint64_t(EntryState::Free));
    } else if (st != std::uint64_t(EntryState::Requested)) {
        reason_ = RejectReason::BadRequest;   // entry changed under us (hostile or sanitized)
        state_ = JoinState::Rejected;
    }
    return state_;
}

void MemberClient::heartbeat() noexcept {
    if (state_ == JoinState::Active) fetchAdd(entry().heartbeat, 1);
}

void MemberClient::leave() noexcept {
    if (state_ != JoinState::Active) return;
    MemberEntry& e = entry();
    if (loadAcquire(e.generation) == generation_)
        compareExchange(e.state, std::uint64_t(EntryState::Active), std::uint64_t(EntryState::Leaving));
}

// ---------------------------------------------------------------------------------------------
// MemberEndpoint (audio)

void MemberEndpoint::bind(std::byte* base, const RegionHeader& hdr, const Layout& l, std::uint32_t slot,
                          const Uuid& member, std::uint64_t generation, std::uint32_t channels) noexcept {
    base_ = base;
    hdr_ = hdr;
    shared_ = reinterpret_cast<RegionHeader*>(base);
    entry_ = reinterpret_cast<MemberEntry*>(base + l.entryOffset + std::uint64_t(slot) * l.entryStride);
    std::byte* blk = base + l.ringsOffset + std::uint64_t(slot) * l.memberStride;
    up_ = ShmRing(blk + l.upRingOffset, blk + l.upRingOffset + sizeof(RingCtl), sizeof(UpFrame));
    down_ = ShmRing(blk + l.downRingOffset, blk + l.downRingOffset + sizeof(RingCtl), sizeof(DownFrame));
    member_ = member;
    generation_ = generation;
    channels_ = std::clamp(channels, 1u, kMaxChannels);
    latencyQuanta_ = hdr.latencyQuanta;
    timeoutQuanta_ = std::int64_t(std::ceil(kLeaseMs * 1e-3 * hdr.sampleRate / double(kQuantum)));
    railDecay_ = std::exp(-(double(kQuantum) / hdr.sampleRate) / kMissingDemandTauS);
    wStep_ = 1.0 / (kInfluenceRampS * hdr.sampleRate);
    lastBrokerHb_ = loadAcquire(shared_->brokerHeartbeat);
    bound_.store(true, std::memory_order_release);
}

bool MemberEndpoint::downPayloadValid(const DownPayload& p, std::int64_t tick) const noexcept {
    if (p.tick != tick) return false;
    if (!std::isfinite(p.railV) || p.railV < 0 || p.railV > 2 * hdr_.nominalRailV) return false;
    if (p.activeMembers > kMaxMembers) return false;
    const double lim = kMaxBleedGain * kTapLimit;
    for (std::uint32_t c = 0; c < channels_; ++c)
        for (std::uint32_t n = 0; n < kQuantum; ++n)
            if (!(std::fabs(p.bleed[c][n]) <= lim)) return false;
    return true;
}

void MemberEndpoint::fetchResult(std::int64_t tick) noexcept {
    resValid_ = false;
    for (std::uint32_t k = 0; k < kRingSlots; ++k) {
        if (!down_.peek(&scratch_)) break;
        const Envelope& e = scratch_.env;
        if (!envelopeValid(e, PayloadType::Down, sizeof(DownFrame), sizeof(DownPayload), hdr_, member_, generation_) ||
            e.channels != channels_ || !downPayloadValid(scratch_.p, e.sampleStart / std::int64_t(kQuantum))) {
            down_.pop();
            ++diag_.invalidFrames;
            callStatus_ |= netstatus::kInvalidFrame;
            continue;
        }
        const std::int64_t t = scratch_.p.tick;
        if (t < tick) { down_.pop(); ++diag_.staleDropped; callStatus_ |= netstatus::kStaleDropped; continue; }
        if (t > tick + std::int64_t(2 * latencyQuanta_)) {   // outside the window: pre-seek leftovers
            down_.pop(); ++diag_.staleDropped; callStatus_ |= netstatus::kStaleDropped; continue;
        }
        if (t > tick) break;   // early result: keep it for its own quantum
        for (std::uint32_t c = 0; c < channels_; ++c) std::memcpy(bleed_[c], scratch_.p.bleed[c], sizeof bleed_[c]);
        railDev_ = scratch_.p.railV - hdr_.nominalRailV;
        if (scratch_.p.flags & tickflag::kDegraded) callStatus_ |= netstatus::kDegraded;
        down_.pop();
        resValid_ = true;
        break;
    }
    if (down_.corrupt()) diag_.ringCorrupt = true;
    if (resValid_) {
        ++diag_.resultsUsed;
    } else {
        ++diag_.resultsMissing;
        railDev_ *= railDecay_;   // same decay rule as the broker (100 ms tau); zero bleed
        callStatus_ |= netstatus::kDegraded;
    }
}

void MemberEndpoint::onQuantum(std::int64_t q) noexcept {
    ++diag_.quanta;
    // Broker liveness from the heartbeat word, measured in processed audio (no clock syscalls).
    const std::uint64_t hb = loadAcquire(shared_->brokerHeartbeat);
    if (hb != lastBrokerHb_) { lastBrokerHb_ = hb; staleQuanta_ = 0; }
    else if (++staleQuanta_ > timeoutQuanta_) brokerLost_ = true;
    if (loadAcquire(shared_->brokerState) == std::uint64_t(BrokerState::Closed)) brokerLost_ = true;
    const std::uint64_t st = loadAcquire(entry_->state);
    if (loadAcquire(entry_->generation) != generation_ ||
        (st != std::uint64_t(EntryState::Active) && st != std::uint64_t(EntryState::Leaving)))
        evicted_ = true;
    leaving_ = st == std::uint64_t(EntryState::Leaving);
    if (!brokerLost_ && !evicted_) fetchAdd(entry_->heartbeat, 1);
    if (up_.corrupt() || down_.corrupt()) diag_.ringCorrupt = true;
    if (!brokerLost_ && !evicted_ && !diag_.ringCorrupt) fetchResult(q - std::int64_t(latencyQuanta_));
    else resValid_ = false;
}

void MemberEndpoint::publish(std::int64_t q, double powerW, double localRailV) noexcept {
    UpFrame& f = upScratch_;
    std::memset(&f, 0, sizeof f);
    fillEnvelope(f.env, PayloadType::Up, sizeof(UpFrame), sizeof(UpPayload), hdr_, member_, generation_, channels_,
                 q * std::int64_t(kQuantum), sequence_++);
    const double v = (std::isfinite(localRailV) && localRailV > 0) ? localRailV : hdr_.nominalRailV;
    const double p = (std::isfinite(powerW) && powerW > 0) ? std::min(powerW, 1e6) : 0.0;
    f.p.powerW = p;
    f.p.demandA = std::min(p / v, 1e3);   // synthetic demand model I = P / V_rail
    f.p.activity = std::clamp(std::sqrt(upEnergy_ / double(kQuantum * channels_)), 0.0, 1.0);
    f.p.railModelId = hdr_.railModelId;
    for (std::uint32_t c = 0; c < channels_; ++c) std::memcpy(f.p.tap[c], upTap_[c], sizeof upTap_[c]);
    if (up_.push(&f)) ++diag_.published;
    else { ++diag_.upOverflow; callStatus_ |= netstatus::kUpOverflow; }
}

NetResult MemberEndpoint::process(const NetContext& ctx, const double* const* tap, double* const* bleedOut) noexcept {
    NetResult r;
    const std::uint32_t frames = ctx.frames;
    if (!bound()) {
        for (std::uint32_t c = 0; c < channels_; ++c)
            if (bleedOut && bleedOut[c]) std::fill(bleedOut[c], bleedOut[c] + frames, 0.0);
        r.status = netstatus::kUnbound | netstatus::kUnavailable;
        r.railV = ctx.localRailV;
        return r;
    }
    callStatus_ = 0;
    std::uint32_t transient = 0;
    if (ctx.offline) transient |= netstatus::kOffline;
    if (!(ctx.sampleRate == hdr_.sampleRate)) transient |= netstatus::kRateMismatch;
    if (ctx.epoch != hdr_.timelineEpoch) transient |= netstatus::kEpochMismatch;
    if (ctx.sampleStart < 0) transient |= netstatus::kNegativeTime;
    const bool live = transient == 0;   // realtime coupling only for an online, matching timeline
    const double local = (std::isfinite(ctx.localRailV) && ctx.localRailV > 0) ? ctx.localRailV : hdr_.nominalRailV;

    std::uint32_t pos = 0;
    while (pos < frames) {
        const std::int64_t s = ctx.sampleStart + std::int64_t(pos);
        if (!live) {   // no timeline coupling: fade to local, zero bleed, publish nothing
            const std::uint32_t n = frames - pos;
            for (std::uint32_t f = 0; f < n; ++f) w_ = std::max(0.0, w_ - wStep_);
            for (std::uint32_t c = 0; c < channels_; ++c) std::fill(bleedOut[c] + pos, bleedOut[c] + frames, 0.0);
            haveNext_ = false;
            upValid_ = false;
            curQ_ = -1;
            break;
        }
        if (!haveNext_ || s != expectedNext_) {   // first call, seek or loop: restart quantum framing
            if (haveNext_) { ++diag_.seeks; callStatus_ |= netstatus::kSeek; }
            curQ_ = -1;
            upValid_ = false;
        }
        const std::int64_t q = s / std::int64_t(kQuantum);
        const std::uint32_t off = std::uint32_t(s % std::int64_t(kQuantum));
        const std::uint32_t n = std::min<std::uint32_t>(frames - pos, kQuantum - off);
        if (q != curQ_) {
            curQ_ = q;
            upFill_ = 0;
            upEnergy_ = 0;
            upValid_ = off == 0;   // a quantum entered mid-way after a seek is not published
            onQuantum(q);
        }
        const bool ok = !brokerLost_ && !evicted_ && !diag_.ringCorrupt;
        const double target = (ok && !leaving_) ? 1.0 : 0.0;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const double* in = tap ? tap[c] : nullptr;
            for (std::uint32_t f = 0; f < n; ++f) {
                double x = in ? in[pos + f] : 0.0;
                if (!std::isfinite(x)) x = 0.0;
                x = std::clamp(x, -kTapLimit, kTapLimit);
                upTap_[c][off + f] = x;
                upEnergy_ += x * x;
            }
        }
        for (std::uint32_t f = 0; f < n; ++f) {
            if (w_ < target) w_ = std::min(target, w_ + wStep_);
            else if (w_ > target) w_ = std::max(target, w_ - wStep_);
            for (std::uint32_t c = 0; c < channels_; ++c)
                bleedOut[c][pos + f] = resValid_ ? w_ * bleed_[c][off + f] : 0.0;
        }
        upFill_ = off + n;
        if (upFill_ == kQuantum && upValid_ && ok) publish(q, ctx.powerW, local);
        pos += n;
        expectedNext_ = s + std::int64_t(n);
        haveNext_ = true;
    }

    diag_.brokerLost = brokerLost_;
    diag_.evicted = evicted_;
    diag_.influence = w_;
    diag_.sharedRailV = hdr_.nominalRailV + railDev_;
    std::uint32_t st = callStatus_ | transient;
    if (brokerLost_) st |= netstatus::kBrokerLost;
    if (evicted_) st |= netstatus::kEvicted;
    if (diag_.ringCorrupt) st |= netstatus::kRingCorrupt;
    if (!live || brokerLost_ || evicted_ || diag_.ringCorrupt) st |= netstatus::kUnavailable;
    else st |= netstatus::kConnected;
    if (w_ > 0.0 && w_ < 1.0) st |= netstatus::kFading;
    r.status = st;
    r.influence = w_;
    r.railV = (1.0 - w_) * local + w_ * (hdr_.nominalRailV + railDev_);
    return r;
}

}  // namespace cd::network
