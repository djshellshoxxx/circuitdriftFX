// CircuitDrift network: broker core (see GroupBroker.h). Non-audio.
#include "network/GroupBroker.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include "community/Sha256.h"

namespace cd::network {

std::int64_t SteadyBrokerClock::nowMicros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* toString(GroupError e) noexcept {
    switch (e) {
        case GroupError::None: return "ok";
        case GroupError::TooManyGroups: return "broker already hosts the maximum of 8 groups";
        case GroupError::EmptyName: return "group needs a human-readable name";
        case GroupError::EmptySessionKey: return "group needs an explicit session key";
        case GroupError::InvalidSampleRate: return "invalid group sample rate";
        case GroupError::InvalidPsu: return "invalid PSU manifest";
        case GroupError::InvalidBleed: return "invalid bleed level or topology";
    }
    return "unknown";
}

const char* toString(JoinReject e) noexcept {
    switch (e) {
        case JoinReject::None: return "ok";
        case JoinReject::NoSuchGroup: return "no such group";
        case JoinReject::SessionKeyMismatch: return "session key does not match the group";
        case JoinReject::GroupFull: return "group already has the maximum of 32 members";
        case JoinReject::NilMember: return "member UUID is nil";
        case JoinReject::DuplicateMember: return "member already joined";
        case JoinReject::SampleRateMismatch: return "member sample rate differs from the group";
        case JoinReject::PsuManifestMismatch: return "member PSU manifest differs from the group (rails are not interchangeable)";
        case JoinReject::RailModelMismatch: return "member rail model differs from the group";
        case JoinReject::NominalRailMismatch: return "member nominal rail differs from the group";
        case JoinReject::InvalidChannels: return "member must be mono or stereo";
        case JoinReject::AllocationFailed: return "prepared ring allocation failed";
    }
    return "unknown";
}

GroupBroker::GroupBroker(RingAllocator& alloc, BrokerClock& clock, std::uint64_t uuidSeed)
    : alloc_(alloc), clock_(clock), rng_(uuidSeed) {}

GroupBroker::~GroupBroker() {
    for (auto& g : groups_)
        for (auto& m : g->members)
            m->channel->control().brokerState.store(static_cast<std::uint32_t>(BrokerState::Gone),
                                                    std::memory_order_release);
}

GroupBroker::Group* GroupBroker::find(const Uuid128& id) noexcept {
    for (auto& g : groups_) if (g->id == id) return g.get();
    return nullptr;
}
const GroupBroker::Group* GroupBroker::find(const Uuid128& id) const noexcept {
    for (const auto& g : groups_) if (g->id == id) return g.get();
    return nullptr;
}

CreateGroupResult GroupBroker::createGroup(const GroupSpec& spec) {
    CreateGroupResult r;
    auto fail = [&](GroupError e) { r.error = e; r.reason = toString(e); return r; };
    if (groups_.size() >= kMaxGroupsPerBroker) return fail(GroupError::TooManyGroups);
    if (spec.name.empty()) return fail(GroupError::EmptyName);
    if (spec.sessionKey.empty()) return fail(GroupError::EmptySessionKey);
    if (!(spec.sampleRate >= 8000 && spec.sampleRate <= 768000)) return fail(GroupError::InvalidSampleRate);
    if (!validPsuSpec(spec.psu)) return fail(GroupError::InvalidPsu);
    if (const BleedError be = validateBleed(spec.bleed); be != BleedError::None) {
        r = fail(GroupError::InvalidBleed);
        r.reason += std::string(": ") + toString(be);
        return r;
    }
    auto g = std::make_unique<Group>();
    g->spec = spec;
    do { g->id = makeUuidV4(rng_); } while (find(g->id));
    g->session = makeUuidV4(rng_);
    g->keyDigest = community::sha256(spec.sessionKey);
    g->psu = ReservoirPsu(spec.psu, spec.sampleRate);
    g->stats.exists = true;
    g->stats.name = spec.name;
    g->stats.generation = g->generation;
    r.group = g->id;
    r.session = g->session;
    groups_.push_back(std::move(g));
    return r;
}

JoinResult GroupBroker::join(const Uuid128& groupId, std::string_view sessionKey, const MemberDescriptor& d) {
    JoinResult r;
    auto fail = [&](JoinReject e) { r.reject = e; r.reason = toString(e); return r; };
    Group* g = find(groupId);
    if (!g) return fail(JoinReject::NoSuchGroup);
    if (community::sha256(sessionKey) != g->keyDigest) return fail(JoinReject::SessionKeyMismatch);
    if (d.member.isNil()) return fail(JoinReject::NilMember);
    for (const auto& m : g->members) if (m->desc.member == d.member) return fail(JoinReject::DuplicateMember);
    if (g->members.size() >= kMaxMembersPerGroup) return fail(JoinReject::GroupFull);
    if (!(d.sampleRate == g->spec.sampleRate)) return fail(JoinReject::SampleRateMismatch);
    if (d.psuManifestId != g->spec.psu.manifestId) return fail(JoinReject::PsuManifestMismatch);
    if (d.railModelId != g->spec.psu.railModelId) return fail(JoinReject::RailModelMismatch);
    if (!(std::fabs(d.nominalRailV - g->spec.psu.nominalRailV) <= 1e-9 * g->spec.psu.nominalRailV))
        return fail(JoinReject::NominalRailMismatch);
    if (d.channels != 1 && d.channels != 2) return fail(JoinReject::InvalidChannels);

    ChannelIdentity id;
    id.group = g->id;
    id.session = g->session;
    id.member = d.member;
    id.joinGeneration = g->generation + 1;
    id.sampleRate = g->spec.sampleRate;
    id.channels = d.channels;
    id.railModelId = g->spec.psu.railModelId;
    auto m = std::make_unique<Member>();
    try {
        m->channel = std::make_shared<MemberChannel>(alloc_, id);
    } catch (const std::exception&) {
        return fail(JoinReject::AllocationFailed);
    }
    m->desc = d;
    m->joinGeneration = id.joinGeneration;
    m->lastSeenUs = clock_.nowMicros();
    m->pending.resize(kRingQuanta);
    r.channel = m->channel;
    g->generation = id.joinGeneration;
    r.generation = g->generation;
    auto pos = std::lower_bound(g->members.begin(), g->members.end(), d.member,
                                [](const std::unique_ptr<Member>& a, const Uuid128& u) { return a->desc.member < u; });
    g->members.insert(pos, std::move(m));
    rosterChanged(*g);
    return r;
}

bool GroupBroker::leave(const Uuid128& groupId, const Uuid128& member) {
    Group* g = find(groupId);
    if (!g) return false;
    for (auto& m : g->members)
        if (m->desc.member == member) {
            if (!m->leaving) { m->leaving = true; m->leaveUs = clock_.nowMicros(); }
            return true;
        }
    return false;
}

void GroupBroker::rosterChanged(Group& g) {
    const std::size_t n = g.members.size();
    std::vector<Uuid128> ids;
    ids.reserve(n);
    for (const auto& m : g.members) ids.push_back(m->desc.member);
    (void)buildBleedWeights(g.spec.bleed, ids, g.weights);   // validated at createGroup
    g.sources.assign(n, BleedSource{});
    g.sinks.assign(n, BleedSink{});
    g.slots.assign(n, nullptr);
    g.stats.roster = ids;
    g.stats.generation = g.generation;
}

void GroupBroker::adoptEpoch(Group& g, std::uint64_t epoch) {
    g.epoch = epoch;
    g.epochSeen = true;
    g.tickValid = false;
    for (auto& m : g.members) {
        m->started = false;
        for (auto& p : m->pending) p.valid = false;
    }
    g.stats.epoch = epoch;
}

void GroupBroker::drain(Group& g, std::int64_t now) {
    for (auto& mp : g.members) {
        Member& m = *mp;
        const std::uint64_t hb = m.channel->control().memberHeartbeat.load(std::memory_order_acquire);
        if (hb != m.lastHeartbeat) { m.lastHeartbeat = hb; m.lastSeenUs = now; }
        auto& ring = m.channel->uplink();
        const std::size_t n = ring.acquire();
        std::size_t used = 0;
        for (; used < n; ++used) {
            const DemandFrame& f = ring.at(used);
            FrameExpectation x;
            x.type = PayloadType::Demand;
            x.group = g.id;
            x.session = g.session;
            x.member = m.desc.member;
            x.sampleRate = g.spec.sampleRate;
            x.epoch = f.header.epoch;   // epoch policy below
            x.minGeneration = m.joinGeneration;
            FrameError e = validate(f.header, x);
            if (e == FrameError::None && f.header.channels != m.desc.channels) e = FrameError::InvalidChannels;
            if (e == FrameError::None) {
                if (!g.epochSeen || f.header.epoch > g.epoch) adoptEpoch(g, f.header.epoch);
                else if (f.header.epoch < g.epoch) e = FrameError::EpochMismatch;
            }
            if (e != FrameError::None) {
                ++g.stats.rejectedFrames;
                ++g.stats.rejectedByReason[static_cast<std::size_t>(e)];
                continue;
            }
            if (f.payload.railModelId != g.spec.psu.railModelId || f.header.sampleStart % kQuantumFrames != 0 ||
                f.payload.frames != kQuantumFrames) {
                ++g.stats.rejectedFrames;
                continue;
            }
            const std::uint64_t tick = f.header.sampleStart / kQuantumFrames;
            if (!g.tickValid) { g.tickValid = true; g.nextTick = tick; }
            if (tick < g.nextTick) { ++g.stats.lateFrames; continue; }
            if (tick >= g.nextTick + kRingQuanta) break;   // keep in ring until the window moves
            Pending& p = m.pending[tick % kRingQuanta];
            p.valid = true;
            p.tick = tick;
            p.arrivalUs = now;
            p.payload = f.payload;
            auto clean = [](double v) { return std::isfinite(v) ? v : 0.0; };
            p.payload.currentA = std::max(0.0, clean(p.payload.currentA));
            p.payload.powerW = clean(p.payload.powerW);
            for (auto& ch : p.payload.tap)
                for (double& s : ch) s = clean(s);
            if (!m.started || tick < m.firstTick) { m.started = true; m.firstTick = tick; }
            m.lastSeenUs = now;
        }
        ring.release(used);
    }
}

bool GroupBroker::advanceOne(Group& g, std::int64_t now) {
    if (!g.tickValid) return false;
    const std::uint64_t k = g.nextTick;
    const std::size_t slotIndex = k % kRingQuanta;
    std::uint32_t required = 0, present = 0;
    std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
    std::uint64_t minPendingTick = std::numeric_limits<std::uint64_t>::max();
    for (const auto& m : g.members) {
        for (const auto& p : m->pending)
            if (p.valid && p.tick >= k) {
                earliest = std::min(earliest, p.arrivalUs);
                minPendingTick = std::min(minPendingTick, p.tick);
            }
        if (m->started && m->firstTick <= k) {
            ++required;
            const Pending& p = m->pending[slotIndex];
            if (p.valid && p.tick == k) ++present;
        }
    }
    if (required == 0) {
        if (minPendingTick != std::numeric_limits<std::uint64_t>::max() && minPendingTick > k) {
            g.nextTick = minPendingTick;   // nobody was playing at k
            return true;
        }
        return false;
    }
    const bool all = present == required;
    const auto deadlineUs = static_cast<std::int64_t>(kTickDeadlineSeconds * 1e6);
    if (!all && (earliest == std::numeric_limits<std::int64_t>::max() || now - earliest < deadlineUs)) return false;

    const double fs = g.spec.sampleRate;
    const double rampStep = double(kQuantumFrames) / (kInfluenceRampSeconds * fs);
    const double decay = std::exp(-double(kQuantumFrames) / (fs * kDemandDecayTauSeconds));
    const std::size_t n = g.members.size();
    TickRecord& rec = g.stats.lastTick;
    rec.members.resize(n);
    double total = 0;
    for (std::size_t j = 0; j < n; ++j) {
        Member& m = *g.members[j];
        MemberTickInfo& info = rec.members[j];
        info = MemberTickInfo{};
        info.member = m.desc.member;
        g.sources[j] = BleedSource{};
        if (!(m.started && m.firstTick <= k)) continue;
        info.participating = true;
        const double target = m.leaving ? 0.0 : 1.0;
        m.contribution = m.contribution < target ? std::min(target, m.contribution + rampStep)
                                                 : std::max(target, m.contribution - rampStep);
        Pending& p = m.pending[slotIndex];
        info.present = p.valid && p.tick == k;
        if (info.present) {
            m.lastDemandA = p.payload.currentA;
            g.sources[j].channels = m.desc.channels;
            for (std::uint32_t c = 0; c < m.desc.channels; ++c) g.sources[j].ch[c] = p.payload.tap[c];
        } else {
            m.lastDemandA *= decay;   // missing: zero new audio, decayed last demand
        }
        g.sources[j].gain = m.desc.bleedOptIn ? m.contribution : 0.0;
        info.demandA = m.lastDemandA;
        info.contribution = m.contribution;
        total += m.contribution * m.lastDemandA;
    }
    const double r = g.psu.stepQuantum(total);
    for (std::size_t i = 0; i < n; ++i) {
        Member& m = *g.members[i];
        ResultFrame* f = m.channel->downlink().beginWrite();
        if (!f) { ++g.stats.downlinkDrops; f = &g.scratch; }
        g.slots[i] = f;
        BleedSink& s = g.sinks[i];
        s = BleedSink{};
        s.channels = m.desc.channels;
        s.enabled = m.desc.bleedOptIn && rec.members[i].participating;
        for (std::uint32_t c = 0; c < kMaxChannels; ++c) s.ch[c] = f->payload.bleed[c];
    }
    mixBleed(g.weights, n, g.sources, g.sinks, kQuantumFrames);
    for (std::size_t i = 0; i < n; ++i) {
        Member& m = *g.members[i];
        ResultFrame* f = g.slots[i];
        stampHeader<ResultPayload>(f->header);
        f->header.group = g.id;
        f->header.session = g.session;
        f->header.member = m.desc.member;
        f->header.generation = g.generation;
        f->header.sampleRate = g.spec.sampleRate;
        f->header.channels = m.desc.channels;
        f->header.epoch = g.epoch;
        f->header.sampleStart = k * kQuantumFrames;
        f->header.sequence = g.stats.ticksAdvanced;
        f->payload.railFraction = r;
        f->payload.railVolts = g.psu.volts();
        f->payload.totalCurrentA = total;
        f->payload.flags = all ? 0u : kResultDegraded;
        f->payload.activeMembers = required;
        f->payload.presentMembers = present;
        f->payload.frames = kQuantumFrames;
        if (f != &g.scratch) m.channel->downlink().commit();
        Pending& p = m.pending[slotIndex];
        if (p.tick == k) p.valid = false;
    }
    rec.epoch = g.epoch;
    rec.tick = k;
    rec.degraded = !all;
    rec.active = required;
    rec.present = present;
    rec.totalDemandA = total;
    rec.railVolts = g.psu.volts();
    rec.railFraction = r;
    ++g.stats.ticksAdvanced;
    if (!all) ++g.stats.degradedTicks;
    g.nextTick = k + 1;
    return true;
}

void GroupBroker::expire(Group& g, std::int64_t now) {
    const auto timeoutUs = static_cast<std::int64_t>(kHeartbeatTimeoutSeconds * 1e6);
    const auto leaveUs = static_cast<std::int64_t>(2 * kInfluenceRampSeconds * 1e6);
    bool changed = false;
    for (auto it = g.members.begin(); it != g.members.end();) {
        Member& m = **it;
        const bool timedOut = now - m.lastSeenUs > timeoutUs;
        const bool left = m.leaving && (m.contribution <= 0.0 || !m.started || now - m.leaveUs >= leaveUs);
        if (timedOut || left) {
            m.channel->control().membership.store(static_cast<std::uint32_t>(MembershipState::Removed),
                                                  std::memory_order_release);
            if (timedOut) ++g.stats.removedByTimeout;
            it = g.members.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        ++g.generation;
        rosterChanged(g);
    }
}

std::uint32_t GroupBroker::pump() {
    const std::int64_t now = clock_.nowMicros();
    std::uint32_t ticks = 0;
    for (auto& gp : groups_) {
        Group& g = *gp;
        drain(g, now);
        for (std::uint32_t i = 0; i < kRingQuanta && advanceOne(g, now); ++i) ++ticks;
        drain(g, now);   // frames held back by the tick window
        expire(g, now);
        g.stats.nextTick = g.nextTick;
    }
    return ticks;
}

GroupStatus GroupBroker::status(const Uuid128& group) const {
    const Group* g = find(group);
    if (!g) return GroupStatus{};
    GroupStatus s = g->stats;
    s.nextTick = g->nextTick;
    s.generation = g->generation;
    return s;
}

}  // namespace cd::network
