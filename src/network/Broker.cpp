// CircuitDrift network: group/local broker implementation (see Broker.h). Owner: cd_network.
// Threading: create()/LocalBroker::createGroup allocate (non-audio); service()/close() run on the
// broker worker, noexcept and allocation-free, bounded per call.
#include "network/Broker.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cd::network {

namespace {
constexpr double kMaxDemandA = 1e3, kMaxPowerW = 1e6;
}

std::unique_ptr<GroupBroker> GroupBroker::create(const GroupConfig& cfgIn, std::unique_ptr<SharedRegion> region,
                                                 std::string& error) {
    GroupConfig cfg = cfgIn;
    if (!region) { error = "no region"; return nullptr; }
    if (cfg.sessionKey.empty()) { error = "session key required"; return nullptr; }
    if (cfg.name.size() > 63) { error = "group name too long"; return nullptr; }
    if (cfg.channels < 1 || cfg.channels > kMaxChannels) { error = "channels must be 1 or 2"; return nullptr; }
    if (!std::isfinite(cfg.sampleRate) || cfg.sampleRate < 8000 || cfg.sampleRate > 768000) { error = "sample rate"; return nullptr; }
    if (cfg.latencyQuanta < 1 || cfg.latencyQuanta > kRingSlots / 2) { error = "latency quanta"; return nullptr; }
    if (!cfg.psu.valid()) { error = "invalid PSU manifest"; return nullptr; }
    if (!(cfg.deadlineMs > 0) || !(cfg.leaseMs > 0)) { error = "deadline/lease"; return nullptr; }
    Layout l;
    if (!computeLayout(cfg.maxMembers, l)) { error = "members must be 2..32"; return nullptr; }
    if (region->size() < l.totalBytes) { error = "region smaller than layout"; return nullptr; }
    if (cfg.group.isNil()) cfg.group = Uuid::random();
    if (cfg.session.isNil()) cfg.session = Uuid::random();

    std::unique_ptr<GroupBroker> b(new GroupBroker());
    b->cfg_ = cfg;
    b->layout_ = l;
    b->gain_ = bleedGain(cfg.bleed);
    if (!b->psu_.prepare(cfg.psu, cfg.sampleRate)) { error = "PSU prepare failed"; return nullptr; }
    if (cfg.useTopology) {
        for (std::size_t i = 0; i < cfg.topology.size() && i < cfg.maxMembers; ++i)
            for (std::size_t j = 0; j < cfg.topology[i].size() && j < cfg.maxMembers; ++j)
                b->matrix_[i][j] = cfg.topology[i][j];
        b->diag_.rowClamped = enforceRowSums(b->matrix_, cfg.maxMembers, b->gain_);
        b->topo_ = true;
    }
    b->rampStep_ = 1.0 / (kInfluenceRampS * cfg.sampleRate);
    b->demandDecay_ = std::exp(-(double(kQuantum) / cfg.sampleRate) / kMissingDemandTauS);
    b->slots_.resize(cfg.maxMembers);
    for (auto& s : b->slots_) s.pending.resize(kRingSlots);
    b->weights_.assign(std::size_t(cfg.maxMembers) * kQuantum, 0.0);
    b->bleed_.assign(std::size_t(kMaxChannels) * kQuantum, 0.0);

    std::byte* base = region->data();
    std::memset(base, 0, l.totalBytes);
    RegionHeader h{};
    std::memcpy(h.magic, "CDIP", 4);
    h.major = kProtocolMajor;
    h.minor = kProtocolMinor;
    h.headerBytes = sizeof(RegionHeader);
    h.latencyQuanta = cfg.latencyQuanta;
    h.totalBytes = l.totalBytes;
    std::memcpy(h.group, cfg.group.b.data(), 16);
    std::memcpy(h.session, cfg.session.b.data(), 16);
    h.maxMembers = cfg.maxMembers;
    h.ringSlots = kRingSlots;
    h.quantum = kQuantum;
    h.channels = cfg.channels;
    h.sampleRate = cfg.sampleRate;
    h.timelineEpoch = cfg.timelineEpoch;
    h.entryOffset = l.entryOffset;
    h.entryStride = l.entryStride;
    h.ringsOffset = l.ringsOffset;
    h.memberStride = l.memberStride;
    h.upFrameBytes = sizeof(UpFrame);
    h.downFrameBytes = sizeof(DownFrame);
    h.railModelId = cfg.psu.railModelId;
    h.nominalRailV = cfg.psu.nominalV;
    h.psuManifestHash = cfg.psu.hash();
    h.sessionKeyHash = sessionKeyHash(cfg.session, cfg.sessionKey);
    h.bleedSeed = cfg.bleedSeed;
    std::memcpy(h.groupName, cfg.name.data(), cfg.name.size());
    h.immutableHash = hash64(&h, offsetof(RegionHeader, immutableHash));
    h.rosterGeneration = 1;
    h.brokerState = static_cast<std::uint64_t>(BrokerState::Running);
    std::memcpy(base, &h, sizeof h);
    b->hdr_ = h;
    for (std::uint32_t s = 0; s < cfg.maxMembers; ++s) {
        std::byte* blk = base + l.ringsOffset + std::uint64_t(s) * l.memberStride;
        b->slots_[s].up = ShmRing(blk + l.upRingOffset, blk + l.upRingOffset + sizeof(RingCtl), sizeof(UpFrame));
        b->slots_[s].down = ShmRing(blk + l.downRingOffset, blk + l.downRingOffset + sizeof(RingCtl), sizeof(DownFrame));
    }
    b->region_ = std::move(region);
    b->diag_.rosterGeneration = 1;
    b->diag_.gain = b->gain_;
    b->diag_.railV = b->psu_.voltage();
    return b;
}

MemberEntry& GroupBroker::entry(std::uint32_t s) noexcept {
    return *reinterpret_cast<MemberEntry*>(region_->data() + layout_.entryOffset + std::uint64_t(s) * layout_.entryStride);
}

std::uint32_t GroupBroker::rosterSize() const noexcept {
    std::uint32_t n = 0;
    for (const auto& s : slots_) n += s.active ? 1u : 0u;
    return n;
}

double GroupBroker::effectiveRowAbsSum(std::uint32_t slot) const noexcept {
    if (slot >= slots_.size()) return 0;
    if (topo_) {
        double sum = 0;
        for (std::uint32_t j = 0; j < slots_.size(); ++j)
            if (j != slot && slots_[j].active) sum += std::fabs(matrix_[slot][j]);
        return sum;
    }
    const std::uint32_t n = rosterSize();
    if (n < 2) return 0;
    // default topology: (N-1) peers each with |g/(N-1) * p_j| (mono->stereo maps at <= 1)
    return double(n - 1) * (gain_ / double(n - 1));
}

void GroupBroker::close() noexcept {
    storeRelease(sharedHeader().brokerState, static_cast<std::uint64_t>(BrokerState::Closed));
}

RejectReason GroupBroker::checkRequest(std::uint32_t s, const MemberEntry& req) const noexcept {
    Uuid id;
    std::memcpy(id.b.data(), req.member, 16);
    if (id.isNil()) return RejectReason::BadRequest;
    for (std::uint32_t j = 0; j < slots_.size(); ++j)
        if (j != s && slots_[j].active && slots_[j].member == id) return RejectReason::BadRequest;   // duplicate
    if (!(req.sampleRate == cfg_.sampleRate)) return RejectReason::SampleRate;
    if (req.channels < 1 || req.channels > kMaxChannels) return RejectReason::Channels;
    if (req.railModelId != cfg_.psu.railModelId) return RejectReason::RailModel;
    if (!(req.nominalRailV == cfg_.psu.nominalV)) return RejectReason::NominalRail;
    if (req.psuManifestHash != cfg_.psu.hash()) return RejectReason::PsuManifest;
    if (req.timelineEpoch != cfg_.timelineEpoch) return RejectReason::Epoch;
    return RejectReason::None;
}

void GroupBroker::activate(std::uint32_t s, const MemberEntry& req, double nowMs) noexcept {
    Slot& sl = slots_[s];
    const std::uint64_t gen = fetchAdd(sharedHeader().rosterGeneration, 1) + 1;
    sl.active = true;
    sl.leaving = false;
    sl.generation = gen;
    std::memcpy(sl.member.b.data(), req.member, 16);
    sl.channels = req.channels;
    sl.w = 0;
    sl.wTarget = 1;
    sl.lastDemand = 0;
    sl.lastHeartbeat = req.heartbeat;
    sl.lastHeartbeatMs = nowMs;
    sl.sign = memberSign(cfg_.bleedSeed, sl.member);
    sl.sequence = 0;
    for (auto& p : sl.pending) p.tick = -1;
    sl.up.resetShared();
    sl.down.resetShared();
    MemberEntry& e = entry(s);
    storeRelease(e.reason, static_cast<std::uint64_t>(RejectReason::None));
    storeRelease(e.generation, gen);
    storeRelease(e.state, static_cast<std::uint64_t>(EntryState::Active));
    ++diag_.joins;
}

void GroupBroker::freeSlot(std::uint32_t s) noexcept {
    Slot& sl = slots_[s];
    sl.active = false;
    sl.leaving = false;
    sl.w = sl.wTarget = 0;
    for (auto& p : sl.pending) p.tick = -1;
    fetchAdd(sharedHeader().rosterGeneration, 1);
}

void GroupBroker::evict(std::uint32_t s, RejectReason why) noexcept {
    MemberEntry& e = entry(s);
    if (slots_[s].active) freeSlot(s);
    storeRelease(e.reason, static_cast<std::uint64_t>(why));
    storeRelease(e.state, static_cast<std::uint64_t>(EntryState::Rejected));
    ++diag_.evictions;
}

void GroupBroker::scanEntries(double nowMs) noexcept {
    for (std::uint32_t s = 0; s < slots_.size(); ++s) {
        Slot& sl = slots_[s];
        MemberEntry& e = entry(s);
        const std::uint64_t st = loadAcquire(e.state);
        if (st != sl.seenState) { sl.seenState = st; sl.stateSinceMs = nowMs; }
        switch (st) {
            case std::uint64_t(EntryState::Free):
                if (sl.active) freeSlot(s);   // member released its entry without Leaving
                break;
            case std::uint64_t(EntryState::Claimed):
                // A member that died mid-claim must not leak the slot.
                if (nowMs - sl.stateSinceMs > cfg_.leaseMs && compareExchange(e.state, st, std::uint64_t(EntryState::Free)))
                    ++diag_.sanitizedEntries;
                break;
            case std::uint64_t(EntryState::Requested): {
                if (sl.active) { evict(s, RejectReason::BadRequest); break; }
                MemberEntry req;
                std::memcpy(&req, &e, sizeof req);   // private copy before validation
                const RejectReason why = checkRequest(s, req);
                if (why == RejectReason::None) {
                    activate(s, req, nowMs);
                } else {
                    storeRelease(e.reason, static_cast<std::uint64_t>(why));
                    storeRelease(e.state, static_cast<std::uint64_t>(EntryState::Rejected));
                    ++diag_.rejects;
                }
                break;
            }
            case std::uint64_t(EntryState::Active): {
                if (!sl.active || loadAcquire(e.generation) != sl.generation) { evict(s, RejectReason::Evicted); break; }
                const std::uint64_t hb = loadAcquire(e.heartbeat);
                if (hb != sl.lastHeartbeat) { sl.lastHeartbeat = hb; sl.lastHeartbeatMs = nowMs; }
                if (nowMs - sl.lastHeartbeatMs > cfg_.leaseMs || sl.up.corrupt() || sl.down.corrupt())
                    evict(s, RejectReason::Evicted);
                break;
            }
            case std::uint64_t(EntryState::Leaving):
                if (!sl.active) { storeRelease(e.state, std::uint64_t(EntryState::Free)); break; }
                if (!sl.leaving) { sl.leaving = true; sl.wTarget = 0; ++diag_.leaves; }
                // Influence ramps out with ticks; if the member stopped producing, finish by time.
                if (sl.w <= 0.0 || nowMs - sl.stateSinceMs > 2 * kInfluenceRampS * 1000.0 + cfg_.deadlineMs) {
                    freeSlot(s);
                    storeRelease(e.state, std::uint64_t(EntryState::Free));
                }
                break;
            case std::uint64_t(EntryState::Rejected):
                if (sl.active) freeSlot(s);
                // Members read the reason then free the entry; a vanished member's entry is reclaimed.
                if (nowMs - sl.stateSinceMs > cfg_.leaseMs && compareExchange(e.state, st, std::uint64_t(EntryState::Free)))
                    ++diag_.sanitizedEntries;
                break;
            default:   // hostile/unknown state word
                if (sl.active) freeSlot(s);
                storeRelease(e.state, std::uint64_t(EntryState::Free));
                ++diag_.sanitizedEntries;
                break;
        }
    }
}

bool GroupBroker::upPayloadValid(const UpPayload& p, std::uint32_t channels) const noexcept {
    if (!std::isfinite(p.demandA) || p.demandA < 0 || p.demandA > kMaxDemandA) return false;
    if (!std::isfinite(p.powerW) || p.powerW < 0 || p.powerW > kMaxPowerW) return false;
    if (!std::isfinite(p.activity) || p.activity < 0 || p.activity > 1) return false;
    if (p.railModelId != cfg_.psu.railModelId) return false;
    for (std::uint32_t c = 0; c < channels; ++c)
        for (std::uint32_t n = 0; n < kQuantum; ++n)
            if (!(std::fabs(p.tap[c][n]) <= kTapLimit)) return false;   // also rejects NaN
    return true;
}

void GroupBroker::drain(std::uint32_t s, double nowMs) noexcept {
    Slot& sl = slots_[s];
    UpFrame f;
    for (std::uint32_t k = 0; k < kRingSlots; ++k) {
        if (!sl.up.peek(&f)) break;
        sl.up.pop();
        ++diag_.framesIn;
        if (!envelopeValid(f.env, PayloadType::Up, sizeof(UpFrame), sizeof(UpPayload), hdr_, sl.member, sl.generation) ||
            f.env.channels != sl.channels || !upPayloadValid(f.p, sl.channels)) {
            ++diag_.invalidFrames;
            continue;
        }
        const std::int64_t t = f.env.sampleStart / std::int64_t(kQuantum);
        if (nextTick_ < 0) nextTick_ = t;
        if (t < nextTick_) {
            ++diag_.staleFrames;
            // Far behind (beyond 2L): a loop/seek back, not a straggler -> resync candidate.
            if (t < nextTick_ - std::int64_t(2 * hdr_.latencyQuanta) && (resyncTick_ < 0 || t < resyncTick_)) resyncTick_ = t;
            continue;
        }
        if (t >= nextTick_ + std::int64_t(kRingSlots)) {
            ++diag_.aheadFrames;
            if (resyncTick_ < 0 || t < resyncTick_) resyncTick_ = t;
            continue;
        }
        Pending& p = sl.pending[std::size_t(t % std::int64_t(kRingSlots))];
        p.tick = t;
        p.p = f.p;
        FirstSeen& fs = firstSeen_[std::size_t(t % std::int64_t(kRingSlots))];
        if (fs.tick != t) { fs.tick = t; fs.ms = nowMs; }
    }
}

void GroupBroker::advanceTicks(double nowMs) noexcept {
    if (nextTick_ < 0) return;
    for (std::uint32_t iter = 0; iter < kRingSlots; ++iter) {
        bool any = false, all = true;
        std::uint32_t members = 0;
        for (const auto& sl : slots_) {
            if (!sl.active) continue;
            ++members;
            const bool have = sl.pending[std::size_t(nextTick_ % std::int64_t(kRingSlots))].tick == nextTick_;
            any = any || have;
            all = all && have;
        }
        if (members == 0) return;
        // Frames for later ticks also start the deadline of a tick nobody delivered.
        bool later = any;
        double first = 1e300;
        for (std::int64_t t = nextTick_; t < nextTick_ + std::int64_t(kRingSlots); ++t) {
            const FirstSeen& fs = firstSeen_[std::size_t(t % std::int64_t(kRingSlots))];
            if (fs.tick == t) { later = true; first = std::min(first, fs.ms); }
        }
        if (!later) return;
        if (!all && nowMs - first < cfg_.deadlineMs) return;
        computeTick(nextTick_);
        if (!all) ++diag_.degradedTicks;
        ++nextTick_;
    }
}

void GroupBroker::computeTick(std::int64_t k) noexcept {
    const std::size_t idx = std::size_t(k % std::int64_t(kRingSlots));
    const std::uint32_t n = std::uint32_t(slots_.size());
    BleedSource src[kMaxMembers];
    bool degraded = false;
    double totalI = 0;
    const std::uint32_t roster = rosterSize();
    for (std::uint32_t j = 0; j < n; ++j) {
        Slot& sl = slots_[j];
        if (!sl.active) continue;
        double* w = weights_.data() + std::size_t(j) * kQuantum;
        double wSum = 0;
        for (std::uint32_t f = 0; f < kQuantum; ++f) {
            if (sl.w < sl.wTarget) sl.w = std::min(sl.wTarget, sl.w + rampStep_);
            else if (sl.w > sl.wTarget) sl.w = std::max(sl.wTarget, sl.w - rampStep_);
            w[f] = sl.w;
            wSum += sl.w;
        }
        Pending& p = sl.pending[idx];
        const bool present = p.tick == k;
        if (present) {
            sl.lastDemand = p.p.demandA;
            src[j].ch[0] = p.p.tap[0];
            src[j].ch[1] = p.p.tap[1];
        } else {
            sl.lastDemand *= demandDecay_;   // missing: zero new audio, last demand decays (100 ms tau)
            degraded = true;
        }
        src[j].channels = sl.channels;
        src[j].sign = sl.sign;
        src[j].weight = w;
        src[j].present = present;
        totalI += (wSum / double(kQuantum)) * sl.lastDemand;
    }
    const double rail = cfg_.sharedDemand ? psu_.step(totalI) : psu_.voltage();
    double maxRow = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        Slot& sl = slots_[i];
        if (!sl.active) continue;
        DownFrame f;
        std::memset(&f, 0, sizeof f);
        fillEnvelope(f.env, PayloadType::Down, sizeof(DownFrame), sizeof(DownPayload), hdr_, sl.member, sl.generation,
                     sl.channels, k * std::int64_t(kQuantum), sl.sequence++);
        f.p.tick = k;
        f.p.rosterGeneration = loadRelaxed(sharedHeader().rosterGeneration);
        f.p.railV = rail;
        f.p.flags = (degraded ? tickflag::kDegraded : 0u) | (diag_.rowClamped ? tickflag::kRowClamped : 0u);
        f.p.activeMembers = roster;
        if (gain_ > 0 && roster > 1) {
            double* out[kMaxChannels] = {f.p.bleed[0], f.p.bleed[1]};
            mixBleed(src, n, roster, i, sl.channels, gain_, topo_ ? &matrix_ : nullptr, out, kQuantum);
            maxRow = std::max(maxRow, effectiveRowAbsSum(i));
        }
        if (sl.down.push(&f)) ++diag_.resultsOut;
        else ++diag_.resultsDropped;
    }
    for (auto& sl : slots_) sl.pending[idx].tick = -1;
    firstSeen_[idx].tick = -1;
    diag_.maxRowAbsSum = maxRow;
    diag_.railV = rail;
    ++diag_.ticks;
}

void GroupBroker::service(double nowMs) noexcept {
    ++diag_.services;
    fetchAdd(sharedHeader().brokerHeartbeat, 1);
    scanEntries(nowMs);
    resyncTick_ = -1;
    for (std::uint32_t s = 0; s < slots_.size(); ++s)
        if (slots_[s].active) drain(s, nowMs);
    // Transport jump (seek/loop of the whole group): nothing pending in the window but frames far
    // away -> restart the tick clock there (counted). Normal stragglers are just dropped as stale.
    if (resyncTick_ >= 0) {
        bool pendingInWindow = false;
        for (const auto& sl : slots_)
            for (const auto& p : sl.pending) pendingInWindow = pendingInWindow || (sl.active && p.tick >= 0);
        if (!pendingInWindow) {
            nextTick_ = resyncTick_;
            for (auto& fs : firstSeen_) fs.tick = -1;
            ++diag_.resyncs;
        }
    }
    advanceTicks(nowMs);
    diag_.active = rosterSize();
    diag_.rosterGeneration = loadAcquire(sharedHeader().rosterGeneration);
    diag_.nextTick = nextTick_;
}

// ---------------------------------------------------------------------------------------------
GroupBroker* LocalBroker::createGroup(GroupConfig cfg, std::string& error) {
    if (groups_.size() >= kMaxGroupsPerBroker) { error = "local broker already holds 8 groups"; return nullptr; }
    if (cfg.sessionKey.empty()) { error = "session key required"; return nullptr; }
    if (cfg.group.isNil()) cfg.group = Uuid::random();
    if (cfg.session.isNil()) cfg.session = Uuid::random();
    for (const auto& g : groups_)
        if (g->config().group == cfg.group) { error = "group exists"; return nullptr; }
    Layout l;
    if (!computeLayout(cfg.maxMembers, l)) { error = "members must be 2..32"; return nullptr; }
    auto region = factory_(PosixShmRegion::nameForSession(cfg.session), l.totalBytes, error);
    if (!region) return nullptr;
    auto g = GroupBroker::create(cfg, std::move(region), error);
    if (!g) return nullptr;
    groups_.push_back(std::move(g));
    return groups_.back().get();
}

bool LocalBroker::closeGroup(const Uuid& group) noexcept {
    for (auto it = groups_.begin(); it != groups_.end(); ++it)
        if ((*it)->config().group == group) { (*it)->close(); groups_.erase(it); return true; }
    return false;
}

std::string LocalBroker::discover(const Uuid& group, std::string_view sessionKey) const {
    for (const auto& g : groups_)
        if (g->config().group == group && g->config().sessionKey == sessionKey) return g->regionName();
    return {};
}

void LocalBroker::service(double nowMs) noexcept {
    for (auto& g : groups_) g->service(nowMs);
}

}  // namespace cd::network
