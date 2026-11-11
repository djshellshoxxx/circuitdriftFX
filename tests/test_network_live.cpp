// T11 (CD-F11) live instance-network tests: GroupBroker core + NetworkMember audio side against the
// in-process mock transport (HeapRingAllocator, ManualClock). SYNTHETIC supply/demand. Covers
// 2/32 members, group isolation, incompatible membership, reorder/delay/drop of peer callbacks,
// heartbeat removal, broker death/restart, seek, leave, non-realtime fallback. Every member
// process() call runs under RtTrapScope (0 allocations/locks/I-O required). This is NOT host
// qualification: separate processes, real shared memory and the per-host matrix are G05 (open).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include "network/GroupBroker.h"
#include "network/NetworkMember.h"
#include "support/RtTrap.h"

namespace {
using namespace cd::network;
int failures = 0;
long rtViolations = 0;
long rtCalls = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

constexpr double kFs = 48000;
const double kG24 = std::pow(10.0, -24.0 / 20.0);

GroupSpec groupSpec(const char* name, const char* key, double db = -24) {
    GroupSpec s;
    s.name = name;
    s.sessionKey = key;
    s.sampleRate = kFs;
    s.bleed.levelDb = db;
    s.bleed.seed = 2026;
    return s;
}
MemberDescriptor desc(Uuid128 id, std::uint32_t ch, bool bleed = true, double fs = kFs) {
    PsuSpec p;
    MemberDescriptor d;
    d.member = id;
    d.psuManifestId = p.manifestId;
    d.railModelId = p.railModelId;
    d.nominalRailV = p.nominalRailV;
    d.sampleRate = fs;
    d.channels = ch;
    d.bleedOptIn = bleed;
    return d;
}

struct Rig {
    HeapRingAllocator alloc;
    ManualClock clock;
    std::unique_ptr<GroupBroker> broker;
    std::uint64_t samples = 0;
    Rig() : broker(std::make_unique<GroupBroker>(alloc, clock, 77)) {}
    void advance(std::uint64_t frames) {
        samples += frames;
        clock.t = static_cast<std::int64_t>(std::llround(double(samples) * 1e6 / kFs));
    }
};

using Gen = std::function<double(std::uint32_t ch, std::uint64_t t)>;
double zero(std::uint32_t, std::uint64_t) { return 0.0; }

struct Inst {
    Uuid128 id;
    std::uint32_t channels;
    NetworkMember m;
    std::shared_ptr<MemberChannel> ch;
    std::uint64_t timeline = 0;
    std::uint64_t epoch = 1;
    bool realtime = true;
    MemberDemand demand{0.1, 30.0, 0.5};
    std::vector<double> hist[2];
    bool finite = true;
    double peak = 0;
    std::uint32_t seenStatus = 0;
    static MemberConfig cfg(Uuid128 id, std::uint32_t c) {
        MemberConfig k;
        k.member = id;
        k.sampleRate = kFs;
        k.channels = c;
        return k;
    }
    Inst(Uuid128 i, std::uint32_t c) : id(i), channels(c), m(cfg(i, c)) {}
    bool join(GroupBroker& b, const Uuid128& g, const char* key, bool bleed = true) {
        auto r = b.join(g, key, desc(id, channels, bleed));
        if (!r.ok()) return false;
        ch = r.channel;
        return m.connect(ch.get());
    }
    void run(std::uint32_t frames, const Gen& gen = zero, bool keep = true) {
        std::vector<double> in[2], out[2];
        const double* ip[2];
        double* op[2];
        for (std::uint32_t c = 0; c < 2; ++c) {
            in[c].resize(frames);
            out[c].assign(frames, 0.0);
            for (std::uint32_t s = 0; s < frames; ++s) in[c][s] = gen(c, timeline + s);
            ip[c] = in[c].data();
            op[c] = out[c].data();
        }
        MemberBlock b;
        b.tap = ip;
        b.channels = channels;
        b.frames = frames;
        b.epoch = epoch;
        b.timelineSample = timeline;
        b.realtime = realtime;
        b.demand = demand;
        b.bleedOut = op;
        {
            cd::test::RtTrapScope trap;
            m.process(b);
            rtViolations += trap.counts().violations();
        }
        ++rtCalls;
        seenStatus |= m.status();
        for (std::uint32_t c = 0; c < channels; ++c)
            for (double v : out[c]) {
                if (!std::isfinite(v)) finite = false;
                peak = std::max(peak, std::fabs(v));
                if (keep) hist[c].push_back(v);
            }
        if (!std::isfinite(m.frame().railFraction)) finite = false;
        timeline += frames;
    }
};

// Two members: exact L*128 delay, g*p_j gain, no own-bleed recursion, clean status.
void twoMembers() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Two", "key-two"));
    CHECK(g.error == GroupError::None && !g.group.isNil() && !g.session.isNil());
    Inst a({0xA, 1}, 2), b({0xB, 1}, 2);
    CHECK(a.join(*rig.broker, g.group, "key-two") && b.join(*rig.broker, g.group, "key-two"));
    const std::uint64_t impulseAt = 128 * 40 + 5;
    const Gen imp = [&](std::uint32_t c, std::uint64_t t) { return c == 0 && t == impulseAt ? 1.0 : 0.0; };
    for (int q = 0; q < 80; ++q) {
        a.run(128, imp);
        b.run(128);
        rig.advance(128);
        rig.broker->pump();
    }
    bool aSilent = true;
    for (auto& h : a.hist) for (double v : h) aSilent = aSilent && v == 0.0;
    CHECK(aSilent);   // A's own input never comes back (pre-circuit tap, no recursion)
    const double expect = kG24 * bleedSign(2026, a.id);   // N=2: g/(N-1) * p_A
    int nonzero = 0;
    for (std::size_t s = 0; s < b.hist[0].size(); ++s)
        if (b.hist[0][s] != 0.0) {
            ++nonzero;
            CHECK(s == impulseAt + kLiveLatencyQuanta * kQuantumFrames);
            CHECK(std::fabs(b.hist[0][s] - expect) < 1e-15);
        }
    for (double v : b.hist[1]) CHECK(v == 0.0);   // same-channel stereo
    CHECK(nonzero == 1);
    const GroupStatus st = rig.broker->status(g.group);
    CHECK(st.ticksAdvanced == 80 && st.degradedTicks == 0 && st.roster.size() == 2);
    CHECK((b.m.status() & kNetExact) && !(b.seenStatus & (kNetDegraded | kNetBrokerLost | kNetUnavailable)));
    CHECK(b.m.influence() == 1.0 && b.m.frame().sourceTick + kLiveLatencyQuanta == b.m.frame().tick);
    const PsuSpec p;
    const double rIdeal = (p.sourceV - 0.2 * p.sourceResistanceOhm) / p.nominalRailV;   // 2 x 0.1 A
    CHECK(std::fabs(b.m.frame().sharedRailFraction - st.lastTick.railFraction) < 0.01);
    CHECK(std::fabs(st.lastTick.railFraction - rIdeal) < 0.01 && std::fabs(st.lastTick.totalDemandA - 0.2) < 1e-12);
    CHECK(std::fabs(b.m.frame().railFraction - b.m.frame().sharedRailFraction) < 1e-15);   // influence 1
    CHECK(a.finite && b.finite);
    std::printf("  two members: bleed delay %u samples (display %.3f ms, %s), gain %.6g (g=%.6g)\n",
                kLiveLatencyQuanta * kQuantumFrames, 1e3 * liveDisplayDelaySeconds(kFs), kLiveCouplingLabel, expect, kG24);
}

// 32 members (mono and stereo), white noise: bounded bleed, all exact in lockstep, 33rd rejected.
void thirtyTwoMembers() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Full", "key-full"));
    std::vector<std::unique_ptr<Inst>> m;
    for (std::uint32_t i = 0; i < 32; ++i) {
        m.push_back(std::make_unique<Inst>(Uuid128{0x100 + i, 0x5EED + i}, i % 3 == 0 ? 1u : 2u));
        CHECK(m.back()->join(*rig.broker, g.group, "key-full"));
    }
    Inst extra({0x999, 9}, 2);
    const auto r = rig.broker->join(g.group, "key-full", desc(extra.id, 2));
    CHECK(r.reject == JoinReject::GroupFull && !r.reason.empty() && !r.channel);
    std::mt19937_64 rng(11);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const Gen noise = [&](std::uint32_t, std::uint64_t) { return u(rng); };
    std::vector<std::size_t> order(32);
    for (std::size_t i = 0; i < 32; ++i) order[i] = i;
    for (int q = 0; q < 160; ++q) {
        std::shuffle(order.begin(), order.end(), rng);   // reordered callbacks within a quantum
        for (std::size_t i : order) m[i]->run(128, noise, false);
        rig.advance(128);
        rig.broker->pump();
    }
    const GroupStatus st = rig.broker->status(g.group);
    CHECK(st.degradedTicks == 0 && st.ticksAdvanced == 160 && st.roster.size() == 32);
    double peak = 0;
    for (auto& i : m) {
        CHECK(i->finite && i->m.counters().exact >= 150 && (i->m.status() & kNetExact));
        peak = std::max(peak, i->peak);
    }
    CHECK(peak <= kG24 * (1 + 1e-12));
    CHECK(peak > 0.2 * kG24);
    std::printf("  32 members: peak bleed %.6g = %.3f g (bound <= g)\n", peak, peak / kG24);
}

// Two groups never influence each other; keys and foreign frames are rejected; <= 8 groups.
std::vector<double> isolationRun(bool g1Loud, GroupStatus* g2out, Rig& rig) {
    const auto g1 = rig.broker->createGroup(groupSpec("Alpha", "key-alpha"));
    const auto g2 = rig.broker->createGroup(groupSpec("Beta", "key-beta"));
    Inst p({0x1, 1}, 2), q({0x2, 1}, 2), r({0x3, 1}, 2), s({0x4, 1}, 1);
    CHECK(p.join(*rig.broker, g1.group, "key-alpha") && q.join(*rig.broker, g1.group, "key-alpha"));
    CHECK(r.join(*rig.broker, g2.group, "key-beta") && s.join(*rig.broker, g2.group, "key-beta"));
    CHECK(rig.broker->join(g2.group, "key-alpha", desc({0x5, 1}, 2)).reject == JoinReject::SessionKeyMismatch);
    CHECK(rig.broker->join({0xDEAD, 0xBEEF}, "key-beta", desc({0x5, 1}, 2)).reject == JoinReject::NoSuchGroup);
    p.demand.currentA = q.demand.currentA = g1Loud ? 0.4 : 0.0;
    std::mt19937_64 rng(5);
    std::uniform_real_distribution<double> u(-1, 1);
    const Gen noise = [&](std::uint32_t, std::uint64_t) { return g1Loud ? u(rng) : 0.0; };
    std::vector<double> rails;
    for (int k = 0; k < 60; ++k) {
        p.run(128, noise); q.run(128, noise); r.run(128); s.run(128);
        rig.advance(128);
        rig.broker->pump();
        rails.push_back(r.m.frame().sharedRailFraction);
    }
    for (auto* i : {&r, &s}) for (auto& h : i->hist) for (double v : h) CHECK(v == 0.0);
    if (g1Loud) CHECK(p.peak > 0 && q.peak > 0);
    // A frame stamped for group 1 written into a group-2 ring is rejected before consumption.
    DemandFrame* f = r.ch->uplink().beginWrite();
    CHECK(f != nullptr);
    stampHeader<DemandPayload>(f->header);
    f->header.group = g1.group; f->header.session = g1.session; f->header.member = r.id;
    f->header.generation = 99; f->header.sampleRate = kFs; f->header.channels = 2; f->header.epoch = 1;
    r.ch->uplink().commit();
    rig.broker->pump();
    *g2out = rig.broker->status(g2.group);
    CHECK(g2out->rejectedByReason[static_cast<std::size_t>(FrameError::WrongGroup)] == 1);
    CHECK(std::fabs(g2out->lastTick.totalDemandA - 0.2) < 1e-12);
    return rails;
}
void isolatedSessions() {
    GroupStatus a, b;
    Rig r1, r2;
    const auto loud = isolationRun(true, &a, r1);
    const auto quiet = isolationRun(false, &b, r2);
    CHECK(loud == quiet);   // group-2 rail is bit-identical whatever group 1 does
    for (int i = 0; i < 6; ++i) CHECK(r1.broker->createGroup(groupSpec("More", "k")).error == GroupError::None);
    const auto ninth = r1.broker->createGroup(groupSpec("Ninth", "k"));
    CHECK(ninth.error == GroupError::TooManyGroups && !ninth.reason.empty());
    CHECK(r2.broker->createGroup(groupSpec("", "k")).error == GroupError::EmptyName);
    CHECK(r2.broker->createGroup(groupSpec("x", "")).error == GroupError::EmptySessionKey);
    CHECK(r2.broker->createGroup(groupSpec("x", "k", -12)).error == GroupError::InvalidBleed);
}

void incompatibleMembership() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Compat", "key"));
    auto expectReject = [&](MemberDescriptor d, JoinReject want) {
        const auto r = rig.broker->join(g.group, "key", d);
        CHECK(r.reject == want && !r.channel && r.reason == toString(want));
        std::printf("  rejected: %s\n", r.reason.c_str());
    };
    auto d = desc({0x10, 1}, 2, true, 44100);
    expectReject(d, JoinReject::SampleRateMismatch);
    d = desc({0x10, 1}, 2); d.psuManifestId ^= 1; expectReject(d, JoinReject::PsuManifestMismatch);
    d = desc({0x10, 1}, 2); d.railModelId ^= 1; expectReject(d, JoinReject::RailModelMismatch);
    d = desc({0x10, 1}, 2); d.nominalRailV = 250; expectReject(d, JoinReject::NominalRailMismatch);
    d = desc({0x10, 1}, 3); expectReject(d, JoinReject::InvalidChannels);
    d = desc({}, 2); expectReject(d, JoinReject::NilMember);
    CHECK(rig.broker->join(g.group, "key", desc({0x10, 1}, 2)).ok());
    expectReject(desc({0x10, 1}, 2), JoinReject::DuplicateMember);
    CHECK(rig.broker->status(g.group).roster.size() == 1);
    // A member prepared at another rate refuses the channel.
    MemberConfig c; c.member = {0x11, 1}; c.sampleRate = 44100; c.channels = 2;
    NetworkMember wrong(c);
    const auto ok = rig.broker->join(g.group, "key", desc({0x11, 1}, 2));
    CHECK(ok.ok() && !wrong.connect(ok.channel.get()));
    rig.alloc.failNext(true);
    expectReject(desc({0x12, 1}, 2), JoinReject::AllocationFailed);
}

// Reordered, delayed (host block 384), randomly partitioned and dropped (40 quanta) callbacks.
void reorderDelayDrop() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Chaos", "key"));
    Inst a({0x21, 1}, 2), b({0x22, 1}, 2), c({0x23, 1}, 1), d({0x24, 1}, 2);
    for (Inst* i : {&a, &b, &c, &d}) CHECK(i->join(*rig.broker, g.group, "key"));
    d.demand.currentA = 0.2;
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> u(-1, 1);
    const Gen noise = [&](std::uint32_t, std::uint64_t) { return u(rng); };
    const double decay = std::exp(-128.0 / (kFs * kDemandDecayTauSeconds));
    std::uint32_t bOwed = 0;
    int decayChecks = 0;
    std::uint64_t maxLag = 0;
    for (int step = 0; step < 260; ++step) {
        std::vector<int> order = {0, 1, 2, 3};
        std::shuffle(order.begin(), order.end(), rng);
        for (int who : order) {
            if (who == 0) a.run(128, noise, false);
            if (who == 1 && (bOwed += 128) == 384) { b.run(384, noise, false); bOwed = 0; }
            if (who == 2) { const std::uint32_t s = 1 + std::uint32_t(rng() % 127); c.run(s, noise, false); c.run(128 - s, noise, false); }
            if (who == 3) {
                if (step < 100 || step >= 140) {
                    if (step == 140) d.timeline = std::uint64_t(step) * 128;   // host resumes at the current position
                    d.run(128, noise, false);
                }
            }
        }
        rig.advance(128);
        rig.broker->pump();
        const GroupStatus st = rig.broker->status(g.group);
        maxLag = std::max<std::uint64_t>(maxLag, std::uint64_t(step + 1) - std::min<std::uint64_t>(st.nextTick, step + 1));
        for (const auto& info : st.lastTick.members)
            if (info.member == d.id && info.participating && !info.present && st.lastTick.tick >= 100 && st.lastTick.tick < 140) {
                const double want = 0.2 * std::pow(decay, double(st.lastTick.tick - 99));
                CHECK(std::fabs(info.demandA - want) <= 1e-12 * want + 1e-15);
                ++decayChecks;
            }
    }
    const GroupStatus st = rig.broker->status(g.group);
    CHECK(decayChecks > 5);
    CHECK(st.degradedTicks > 0 && st.roster.size() == 4 && st.removedByTimeout == 0);
    CHECK(st.degradedTicks < 60);
    CHECK(maxLag >= 19 && maxLag <= 22);   // deadline-bounded group lag (50 ms ~ 19 quanta)
    for (Inst* i : {&a, &b, &c, &d}) CHECK(i->finite && i->peak <= kG24 * (1 + 1e-12));
    // Deadline-forced (degraded) results arrive >= 50 ms late, i.e. after the L = 21.3 ms slack of a
    // member that kept up, so such members see them as missing (decayed rule) and discard them.
    CHECK((a.seenStatus & kNetDecayed) && !(a.seenStatus & kNetBrokerLost));
    CHECK(a.m.counters().staleDiscarded > 0 && (a.m.status() & kNetExact) && a.m.influence() == 1.0);
    CHECK(b.m.counters().exact > 150 && c.m.counters().exact > 150);
    CHECK(d.m.counters().timelineJumps == 1 && (d.m.status() & kNetExact));
    std::printf("  chaos: degraded ticks %llu, max group lag %llu quanta, decay checks %d\n",
                (unsigned long long)st.degradedTicks, (unsigned long long)maxLag, decayChecks);
}

// Member-side decay rule: broker silent for a few quanta -> zero bleed, decaying rail excess.
void memberLocalDecay() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Decay", "key"));
    Inst a({0x31, 1}, 2), b({0x32, 1}, 2);
    CHECK(a.join(*rig.broker, g.group, "key") && b.join(*rig.broker, g.group, "key"));
    a.demand.currentA = 0.02;   // A's local rail stays high, the shared rail is lower (B loads it)
    b.demand.currentA = 0.5;
    for (int q = 0; q < 60; ++q) { a.run(128); b.run(128); rig.advance(128); rig.broker->pump(); }
    CHECK(a.m.frame().sharedRailFraction - a.m.frame().localRailFraction < -0.01);
    const double decay = std::exp(-128.0 / (kFs * kDemandDecayTauSeconds));
    // No pump: the L = 8 buffered results are still consumed exactly, then the local rule applies.
    double excess0 = 0;
    for (int i = 1; i <= 7; ++i) {
        a.run(128); b.run(128); rig.advance(128);
        CHECK(a.m.status() & kNetExact);
        excess0 = a.m.frame().sharedRailFraction - a.m.frame().localRailFraction;
    }
    for (int q = 1; q <= 6; ++q) {
        a.run(128); b.run(128); rig.advance(128);
        const NetworkFrame& f = a.m.frame();
        CHECK((f.status & kNetDecayed) && !(f.status & kNetExact) && !(f.status & kNetBrokerLost));
        CHECK(std::fabs((f.sharedRailFraction - f.localRailFraction) - excess0 * std::pow(decay, q)) < 1e-12);
        for (auto& row : f.bleed) for (double v : row) CHECK(v == 0.0);
    }
    for (int q = 0; q < 5; ++q) { a.run(128); b.run(128); rig.advance(128); rig.broker->pump(); }
    CHECK((a.m.status() & kNetExact) && a.m.counters().staleDiscarded > 0 && a.m.influence() == 1.0);
}

// No heartbeat for > 500 ms removes the member and publishes a new roster generation.
void heartbeatTimeout() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Beat", "key"));
    Inst a({0x41, 1}, 2), b({0x42, 1}, 2);
    CHECK(a.join(*rig.broker, g.group, "key") && b.join(*rig.broker, g.group, "key"));
    for (int q = 0; q < 40; ++q) { a.run(128); b.run(128); rig.advance(128); rig.broker->pump(); }
    const std::uint64_t gen0 = rig.broker->status(g.group).generation;
    int removedAt = -1;
    for (int q = 0; q < 300; ++q) {   // B's host stops calling it (GUI closed / track frozen)
        a.run(128); rig.advance(128); rig.broker->pump();
        if (removedAt < 0 && rig.broker->status(g.group).roster.size() == 1) removedAt = q;
    }
    const GroupStatus st = rig.broker->status(g.group);
    CHECK(st.removedByTimeout == 1 && st.generation == gen0 + 1);
    const double removedMs = (removedAt + 1) * 128.0 / kFs * 1e3;
    CHECK(removedMs > 500 && removedMs < 510);
    CHECK((a.m.status() & kNetExact) && a.m.influence() == 1.0 && a.finite);
    b.timeline = a.timeline;
    for (int q = 0; q < 30; ++q) b.run(128);
    CHECK((b.m.status() & kNetRemoved) && (b.m.status() & kNetBrokerLost) && b.m.influence() == 0.0 && b.finite);
    CHECK(b.m.frame().railFraction == b.m.frame().localRailFraction);
    std::printf("  heartbeat: member removed after %.1f ms, generation %llu -> %llu\n", removedMs,
                (unsigned long long)gen0, (unsigned long long)st.generation);
}

// Broker crash (no pumping) and clean exit + restart: 50 ms fade to local PSU, audio continues.
void brokerDeathAndRestart() {
    Rig rig;
    auto g = rig.broker->createGroup(groupSpec("Death", "key"));
    Inst a({0x51, 1}, 2), b({0x52, 1}, 2);
    CHECK(a.join(*rig.broker, g.group, "key") && b.join(*rig.broker, g.group, "key"));
    std::mt19937_64 rng(9);
    std::uniform_real_distribution<double> u(-1, 1);
    const Gen noise = [&](std::uint32_t, std::uint64_t) { return u(rng); };
    for (int q = 0; q < 60; ++q) { a.run(128, noise, false); b.run(128, noise, false); rig.advance(128); rig.broker->pump(); }
    CHECK(a.m.influence() == 1.0);
    // Crash: the broker stops answering. Process A sample by sample to time the fade.
    long fadeStart = -1, fadeEnd = -1;
    for (long s = 0; s < 128 * 120; ++s) {
        a.run(1, noise, false);
        if (s % 128 == 127) b.run(128, noise, false);
        if (fadeStart < 0 && a.m.influence() < 1.0) fadeStart = s;
        if (fadeEnd < 0 && a.m.influence() == 0.0) fadeEnd = s;
    }
    CHECK(fadeStart > 0 && fadeEnd > fadeStart);
    const long fadeLen = fadeEnd - fadeStart + 1;
    CHECK(fadeLen >= 2399 && fadeLen <= 2401);   // 50 ms at 48 kHz
    const double detectQuanta = double(fadeStart) / 128.0;
    // buffered results cover L quanta, then lostThreshold consecutive misses
    CHECK(detectQuanta >= a.m.lostThresholdQuanta() && detectQuanta <= kLiveLatencyQuanta + a.m.lostThresholdQuanta() + 2.0);
    CHECK((a.m.status() & kNetBrokerLost) && a.finite && a.m.frame().railFraction == a.m.frame().localRailFraction);
    std::printf("  broker crash: loss detected after %.1f ms, fade %ld samples (%.2f ms)\n",
                detectQuanta * 128 / kFs * 1e3, fadeLen, fadeLen / kFs * 1e3);
    // Restart: a new broker; the old channel stays mapped until the member releases it.
    auto oldA = a.ch, oldB = b.ch;
    rig.broker.reset();   // clean exit marks every channel Gone (already faded here)
    CHECK(oldA->control().brokerState.load() == static_cast<std::uint32_t>(BrokerState::Gone));
    rig.broker = std::make_unique<GroupBroker>(rig.alloc, rig.clock, 78);
    g = rig.broker->createGroup(groupSpec("Death", "key"));
    CHECK(a.join(*rig.broker, g.group, "key") && b.join(*rig.broker, g.group, "key"));
    CHECK(!a.m.channelReleased(oldA.get()));
    for (int q = 0; q < 60; ++q) { a.run(128, noise, false); b.run(128, noise, false); rig.advance(128); rig.broker->pump(); }
    CHECK(a.m.channelReleased(oldA.get()) && b.m.channelReleased(oldB.get()));
    oldA.reset();
    oldB.reset();
    CHECK((a.m.status() & kNetExact) && a.m.influence() == 1.0 && a.finite && b.finite);
    // Clean exit while coupled: Gone is seen at the next quantum, fade completes within 50 ms + 1 quantum.
    rig.broker.reset();
    long steps = 0;
    while (a.m.influence() > 0.0 && steps < 100000) { a.run(1, noise, false); ++steps; }
    CHECK(steps <= 2400 + 130 && (a.m.status() & kNetBrokerLost));
    for (int q = 0; q < 10; ++q) a.run(128, noise, false);
    CHECK(a.finite);
}

// Seek/loop: new epoch; stale old-epoch frames are rejected before consumption on both sides.
void seekRejectsStale() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Seek", "key"));
    Inst a({0x61, 1}, 2), b({0x62, 1}, 2);
    CHECK(a.join(*rig.broker, g.group, "key") && b.join(*rig.broker, g.group, "key"));
    for (int q = 0; q < 40; ++q) { a.run(128); b.run(128); rig.advance(128); rig.broker->pump(); }
    for (std::uint64_t target : {480000ull, 960050ull}) {
        a.epoch = b.epoch = a.epoch + 1;
        a.timeline = b.timeline = target;
        for (int q = 0; q < 30; ++q) { a.run(128); b.run(128); rig.advance(128); rig.broker->pump(); }
            CHECK((a.m.status() & kNetExact) && a.m.frame().tick == a.timeline / 128);
        CHECK(a.m.frame().sourceTick >= target / 128 && a.m.frame().sourceTick + kLiveLatencyQuanta == a.m.frame().tick);
    }
    CHECK(a.m.counters().rejected[static_cast<std::size_t>(FrameError::EpochMismatch)] > 0);
    CHECK(a.m.counters().epochChanges == 2 && a.finite);
    // An old-epoch demand frame reaching the broker after the seek is rejected.
    DemandFrame* f = a.ch->uplink().beginWrite();
    stampHeader<DemandPayload>(f->header);
    f->header.group = a.ch->identity().group; f->header.session = a.ch->identity().session;
    f->header.member = a.id; f->header.generation = a.ch->identity().joinGeneration;
    f->header.sampleRate = kFs; f->header.channels = 2; f->header.epoch = 1;
    f->payload.railModelId = a.ch->identity().railModelId;
    a.ch->uplink().commit();
    rig.broker->pump();
    const GroupStatus st = rig.broker->status(g.group);
    CHECK(st.epoch == 3 && st.rejectedByReason[static_cast<std::size_t>(FrameError::EpochMismatch)] == 1);
    // A stale-generation frame (from before this membership) is rejected too.
    f = a.ch->uplink().beginWrite();
    *f = DemandFrame{};
    stampHeader<DemandPayload>(f->header);
    f->header.group = a.ch->identity().group; f->header.session = a.ch->identity().session;
    f->header.member = a.id; f->header.generation = 0; f->header.sampleRate = kFs; f->header.channels = 2; f->header.epoch = 3;
    a.ch->uplink().commit();
    rig.broker->pump();
    CHECK(rig.broker->status(g.group).rejectedByReason[static_cast<std::size_t>(FrameError::StaleGeneration)] == 1);
}

// Opt-out: member ramps its influence out over 50 ms, broker ramps contribution and removes it.
void leaveRamps() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Leave", "key"));
    Inst a({0x71, 1}, 2), b({0x72, 1}, 2), c({0x73, 1}, 2);
    for (Inst* i : {&a, &b, &c}) CHECK(i->join(*rig.broker, g.group, "key"));
    for (int q = 0; q < 50; ++q) { a.run(128); b.run(128); c.run(128); rig.advance(128); rig.broker->pump(); }
    const std::uint64_t gen0 = rig.broker->status(g.group).generation;
    a.m.requestLeave();
    int q = 0;
    for (; q < 100 && !a.m.leaveComplete(); ++q) { a.run(128); b.run(128); c.run(128); rig.advance(128); rig.broker->pump(); }
    CHECK(a.m.leaveComplete() && q >= 18 && q <= 20 && a.m.influence() == 0.0);
    CHECK(rig.broker->leave(g.group, a.id));
    for (int k = 0; k < 40; ++k) { a.run(128); b.run(128); c.run(128); rig.advance(128); rig.broker->pump(); }
    const GroupStatus st = rig.broker->status(g.group);
    CHECK(st.roster.size() == 2 && st.generation == gen0 + 1 && st.removedByTimeout == 0);
    CHECK((b.m.status() & kNetExact) && b.m.influence() == 1.0);
    a.m.disconnect();
    a.run(128);
    CHECK(a.m.channelReleased(a.ch.get()));
}

// Host prefetch/offline: Network unavailable, fade to local, zero bleed, nothing published.
void nonRealtimeFallback() {
    Rig rig;
    const auto g = rig.broker->createGroup(groupSpec("Offline", "key"));
    Inst a({0x81, 1}, 2), b({0x82, 1}, 2);
    CHECK(a.join(*rig.broker, g.group, "key") && b.join(*rig.broker, g.group, "key"));
    const Gen dc = [](std::uint32_t, std::uint64_t) { return 0.5; };
    for (int q = 0; q < 50; ++q) { a.run(128); b.run(128, dc); rig.advance(128); rig.broker->pump(); }
    CHECK(a.m.influence() == 1.0 && a.peak > 0);
    a.realtime = false;
    const std::size_t before = a.ch->uplink().sizeApprox();
    for (int q = 0; q < 30; ++q) { a.run(128); b.run(128, dc); }
    CHECK((a.m.status() & kNetUnavailable) && a.m.influence() == 0.0 && a.finite);
    CHECK(a.ch->uplink().sizeApprox() == before);
    CHECK(a.hist[0].back() == 0.0 && a.m.frame().railFraction == a.m.frame().localRailFraction);
}

// Real threads: member audio thread vs broker worker (steady clock). Smoke only, not qualification.
void threadedSmoke() {
    HeapRingAllocator alloc;
    SteadyBrokerClock clock;
    GroupBroker broker(alloc, clock, 1);
    const auto g = broker.createGroup(groupSpec("Threads", "key"));
    Inst a({0x91, 1}, 2), b({0x92, 1}, 1);
    CHECK(a.join(broker, g.group, "key") && b.join(broker, g.group, "key"));
    std::atomic<bool> done{false};
    std::thread worker([&] { while (!done.load()) { broker.pump(); std::this_thread::sleep_for(std::chrono::microseconds(500)); } });
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<double> u(-1, 1);
    const Gen noise = [&](std::uint32_t, std::uint64_t) { return u(rng); };
    for (int q = 0; q < 300; ++q) {
        a.run(128, noise, false);
        b.run(128, noise, false);
        std::this_thread::sleep_for(std::chrono::microseconds(2667));
    }
    done = true;
    worker.join();
    CHECK(a.finite && b.finite && a.m.counters().exact > 100 && a.peak <= kG24 * (1 + 1e-12));
    std::printf("  threaded smoke: exact %llu / %llu quanta\n", (unsigned long long)a.m.counters().exact,
                (unsigned long long)a.m.counters().quanta);
}

}  // namespace

int main() {
    twoMembers();
    thirtyTwoMembers();
    isolatedSessions();
    incompatibleMembership();
    reorderDelayDrop();
    memberLocalDecay();
    heartbeatTimeout();
    brokerDeathAndRestart();
    seekRejectsStale();
    leaveRamps();
    nonRealtimeFallback();
    threadedSmoke();
    CHECK(rtViolations == 0);
    std::printf("  audio-side calls under RtTrapScope: %ld, violations %ld\n", rtCalls, rtViolations);
    std::printf("network live T11: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
