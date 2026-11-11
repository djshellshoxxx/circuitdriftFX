// CircuitDrift tests: T11 live instance network over the in-process arena transport —
// membership/compatibility/isolation, 2 and 32 members with reordered callbacks and arbitrary
// partitions (exact bleed after warm-up), missing callbacks (deadline/degraded), lease eviction,
// leave ramps, broker death/close (zero waits: RtTrap alloc/lock/IO counters), offline/Fs/epoch/
// seek fallbacks, cross-talk row-sum bound. Signals and the PSU are SYNTHETIC fixtures. Live mode
// is checked for exactness only in lockstep schedules; it is not claimed sample-perfect under DAW
// scheduling (D17). Real host process/sandbox qualification is G05 (open). Owner: network agent.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "network/Broker.h"
#include "network/CrossTalk.h"
#include "network/Member.h"
#include "network/SharedRegion.h"
#include "support/RtTrap.h"

namespace {
using namespace cd::network;
using cd::test::RtTrapCounts;
using cd::test::RtTrapScope;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

constexpr double kFs = 48000;
constexpr double kQuantumMs = 1000.0 * kQuantum / kFs;

struct Lcg {
    std::uint64_t s;
    std::uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return std::uint32_t(s >> 33); }
};

double signal(std::uint32_t member, std::uint32_t ch, std::int64_t s) {
    const double f = 90.0 + 37.0 * member;
    return 0.5 * std::sin(2.0 * M_PI * f * double(s) / kFs + 0.3 * ch + 0.11 * member);
}

RtTrapCounts g_trap{};
void addTrap(const RtTrapCounts& c) {
    g_trap.allocations += c.allocations; g_trap.deallocations += c.deallocations;
    g_trap.mutexLocks += c.mutexLocks; g_trap.condWaits += c.condWaits; g_trap.ioCalls += c.ioCalls;
}

struct Net {
    ArenaDirectory dir;
    std::unique_ptr<LocalBroker> broker;
    Net() {
        broker = std::make_unique<LocalBroker>([this](const std::string& n, std::size_t b, std::string& e) -> std::unique_ptr<SharedRegion> {
            auto a = dir.create(n, b, e);
            if (!a) return nullptr;
            return std::make_unique<ArenaView>(a);
        });
    }
};

JoinRequest joinFor(const GroupBroker& g, std::uint32_t idx, std::uint32_t channels = 2) {
    JoinRequest r;
    r.member = Uuid::fromSeed(1000 + idx);
    r.expectedGroup = g.config().group;
    r.sessionKey = g.config().sessionKey;
    r.sampleRate = g.config().sampleRate;
    r.channels = channels;
    r.timelineEpoch = g.config().timelineEpoch;
    r.psu = g.config().psu;
    return r;
}

std::unique_ptr<MemberClient> join(Net& net, GroupBroker& g, const JoinRequest& r, double nowMs, std::string& err) {
    auto c = MemberClient::attach(net.dir.open(g.regionName(), err), r, err);
    if (!c) return nullptr;
    g.service(nowMs);
    return c;
}

// One simulated member: timeline position, recorded tap/bleed and statuses.
struct SimMember {
    std::unique_ptr<MemberClient> client;
    std::uint32_t idx = 0, channels = 2;
    std::int64_t pos = 0;
    std::vector<std::vector<double>> bleed;   // [ch][sample] (absolute sample = index)
    std::uint32_t statusOr = 0;
    std::int64_t calls = 0;
    std::vector<std::uint32_t> partition;
    std::size_t partIdx = 0;
    double maxAbsBleed = 0;
    bool finite = true;
};

void runBlock(SimMember& m, std::uint32_t frames, bool offline = false, double fs = kFs, std::uint64_t epoch = 0) {
    double tapBuf[2][512], outBuf[2][512];
    for (std::uint32_t c = 0; c < m.channels; ++c)
        for (std::uint32_t f = 0; f < frames; ++f) tapBuf[c][f] = signal(m.idx, c, m.pos + f);
    const double* tap[2] = {tapBuf[0], tapBuf[1]};
    double* out[2] = {outBuf[0], outBuf[1]};
    NetContext ctx;
    ctx.sampleRate = fs;
    ctx.sampleStart = m.pos;
    ctx.epoch = epoch;
    ctx.frames = frames;
    ctx.offline = offline;
    ctx.powerW = 10.0 + m.idx;
    NetResult r;
    {
        RtTrapScope trap;
        r = m.client->endpoint().process(ctx, tap, out);
        addTrap(trap.counts());
    }
    m.statusOr |= r.status;
    ++m.calls;
    for (std::uint32_t c = 0; c < m.channels; ++c) {
        auto& b = m.bleed[c];
        if (b.size() < std::size_t(m.pos + frames)) b.resize(std::size_t(m.pos + frames) + 4096, 0.0);
        for (std::uint32_t f = 0; f < frames; ++f) {
            const double v = outBuf[c][f];
            if (!std::isfinite(v)) m.finite = false;
            m.maxAbsBleed = std::max(m.maxAbsBleed, std::fabs(v));
            b[std::size_t(m.pos + f)] = v;
        }
    }
    m.pos += frames;
}

struct Group {
    std::shared_ptr<Net> net = std::make_shared<Net>();
    GroupBroker* g = nullptr;
    std::vector<SimMember> members;
    double nowMs = 0;
    std::int64_t clock = 0;
    Lcg rng{12345};
};

bool makeGroup(Group& G, std::uint32_t n, double bleedDb, std::uint32_t maxMembers = kMaxMembers,
               const std::vector<std::uint32_t>& channels = {}) {
    GroupConfig cfg;
    cfg.name = "t11";
    cfg.sessionKey = "key-" + std::to_string(n);
    cfg.maxMembers = maxMembers;
    cfg.bleed.enabled = true;
    cfg.bleed.levelDb = bleedDb;
    cfg.bleedSeed = 77;
    std::string err;
    G.g = G.net->broker->createGroup(cfg, err);
    CHECK(G.g != nullptr, "createGroup: %s", err.c_str());
    if (!G.g) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        SimMember m;
        m.idx = i;
        m.channels = channels.empty() ? 2 : channels[i % channels.size()];
        m.client = join(*G.net, *G.g, joinFor(*G.g, i, m.channels), G.nowMs, err);
        CHECK(m.client != nullptr, "attach %u: %s", i, err.c_str());
        if (!m.client) return false;
        CHECK(m.client->poll() == MemberClient::JoinState::Active, "member %u not active", i);
        m.bleed.assign(m.channels, {});
        static const std::uint32_t parts[] = {1, 17, 64, 127, 128, 256};
        Lcg r{99 + i};
        for (int k = 0; k < 64; ++k) m.partition.push_back(parts[r.u() % 6]);
        G.members.push_back(std::move(m));
    }
    return true;
}

// One wall-clock quantum: members run in random order with their own partitions until they reach
// the clock (hosts process ahead in bursts); `skip` members are not called at all.
void cycle(Group& G, const std::vector<bool>* skip = nullptr, bool service = true) {
    G.clock += kQuantum;
    G.nowMs += kQuantumMs;
    std::vector<std::size_t> order(G.members.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[G.rng.u() % i]);
    for (std::size_t k : order) {
        if (skip && (*skip)[k]) continue;
        SimMember& m = G.members[k];
        while (m.pos < G.clock) {
            const std::uint32_t p = m.partition[m.partIdx++ % m.partition.size()];
            runBlock(m, p);
        }
    }
    if (service) G.g->service(G.nowMs);
}

// Expected live bleed for receiver i at absolute sample s (all present, weights 1, default topology).
double expectedBleed(const Group& G, std::size_t i, std::uint32_t c, std::int64_t s) {
    const std::int64_t t = s - std::int64_t(G.g->config().latencyQuanta * kQuantum);
    if (t < 0) return 0;
    const std::uint32_t n = std::uint32_t(G.members.size());
    std::vector<const SimMember*> bySlot(kMaxMembers, nullptr);
    for (const auto& m : G.members) bySlot[m.client->slot()] = &m;
    const double k = G.g->gain() / double(std::max(1u, n - 1));
    const std::uint32_t rc = G.members[i].channels;
    const std::uint32_t recvSlot = G.members[i].client->slot();
    double o = 0;
    for (std::uint32_t j = 0; j < kMaxMembers; ++j) {
        const SimMember* m = bySlot[j];
        if (!m || j == recvSlot) continue;
        double x;
        if (m->channels == 1) x = rc == 2 ? signal(m->idx, 0, t) * 0.70710678118654752 : signal(m->idx, 0, t);
        else if (rc == 1) x = 0.5 * (signal(m->idx, 0, t) + signal(m->idx, 1, t));
        else x = signal(m->idx, c, t);
        o += (k * memberSign(G.g->config().bleedSeed, m->client->member())) * 1.0 * x;
    }
    return o;
}

double maxExactError(const Group& G, std::int64_t from, std::int64_t to) {
    double worst = 0;
    for (std::size_t i = 0; i < G.members.size(); ++i)
        for (std::uint32_t c = 0; c < G.members[i].channels; ++c)
            for (std::int64_t s = from; s < to; ++s)
                worst = std::max(worst, std::fabs(G.members[i].bleed[c][std::size_t(s)] - expectedBleed(G, i, c, s)));
    return worst;
}

// ---------------------------------------------------------------------------------------------
void trapPositiveControls() {
    RtTrapCounts a;
    {
        RtTrapScope s;
        void* p = ::operator new(32);
        ::operator delete(p);
        a = s.counts();
    }
    CHECK(a.allocations == 1 && a.deallocations == 1, "alloc trap control %ld/%ld", a.allocations, a.deallocations);
    if (cd::test::kRtTrapLockIoInstrumented) {
        std::mutex mu;
        {
            RtTrapScope s;
            mu.lock();
            mu.unlock();
            a = s.counts();
        }
        CHECK(a.mutexLocks == 1, "lock trap control %ld", a.mutexLocks);
    }
    std::printf("T11 RtTrap positive controls: alloc/free%s trapped\n", cd::test::kRtTrapLockIoInstrumented ? " + mutex" : " (lock/IO NOT INSTRUMENTED)");
}

void membershipAndCompatibility() {
    Net net;
    GroupConfig cfg;
    cfg.sessionKey = "alpha";
    cfg.maxMembers = 3;
    std::string err;
    GroupBroker* g = net.broker->createGroup(cfg, err);
    CHECK(g, "create: %s", err.c_str());
    if (!g) return;
    CHECK(!g->config().group.isNil() && !g->config().session.isNil(), "random ids not generated");
    CHECK(g->regionName().rfind("/cdip-", 0) == 0 && g->regionName().size() == 30, "region name %s", g->regionName().c_str());
    // wrong key / wrong group are refused before any entry is touched
    JoinRequest r = joinFor(*g, 0);
    r.sessionKey = "beta";
    CHECK(!MemberClient::attach(net.dir.open(g->regionName(), err), r, err) && err == "session key mismatch", "wrong key: %s", err.c_str());
    r = joinFor(*g, 0);
    r.expectedGroup = Uuid::fromSeed(5);
    CHECK(!MemberClient::attach(net.dir.open(g->regionName(), err), r, err), "wrong group accepted");
    // incompatible requests rejected by the broker with a per-member reason
    struct Bad { const char* what; RejectReason why; void (*mut)(JoinRequest&); };
    const Bad bads[] = {
        {"Fs", RejectReason::SampleRate, [](JoinRequest& q) { q.sampleRate = 44100; }},
        {"rail model", RejectReason::RailModel, [](JoinRequest& q) { q.psu.railModelId ^= 1; }},
        {"nominal rail", RejectReason::NominalRail, [](JoinRequest& q) { q.psu.nominalV = 12; }},
        {"manifest", RejectReason::PsuManifest, [](JoinRequest& q) { q.psu.capacitanceF *= 2; }},
        {"epoch", RejectReason::Epoch, [](JoinRequest& q) { q.timelineEpoch = 9; }},
    };
    for (const auto& b : bads) {
        JoinRequest q = joinFor(*g, 1);
        b.mut(q);
        auto c = join(net, *g, q, 0, err);
        CHECK(c && c->poll() == MemberClient::JoinState::Rejected && c->reason() == b.why, "%s not rejected (%s)", b.what,
              c ? rejectReasonName(c->reason()) : err.c_str());
    }
    JoinRequest q3 = joinFor(*g, 1, 3);
    CHECK(!MemberClient::attach(net.dir.open(g->regionName(), err), q3, err), "3 channels accepted");
    // 3 members fit, a 4th is refused (group full), duplicate UUID rejected
    std::vector<std::unique_ptr<MemberClient>> ms;
    for (std::uint32_t i = 0; i < 3; ++i) {
        ms.push_back(join(net, *g, joinFor(*g, i), 1, err));
        CHECK(ms.back() && ms.back()->poll() == MemberClient::JoinState::Active, "member %u join", i);
    }
    CHECK(!MemberClient::attach(net.dir.open(g->regionName(), err), joinFor(*g, 7), err) && err == "group full", "4th accepted: %s", err.c_str());
    const auto gen0 = g->diagnostics().rosterGeneration;
    ms[2]->leave();
    for (int k = 0; k < 40; ++k) g->service(2 + k * 5.0);
    CHECK(g->diagnostics().active == 2 && g->diagnostics().rosterGeneration > gen0, "leave not applied (%u)", g->diagnostics().active);
    ms.pop_back();
    auto dup = join(net, *g, joinFor(*g, 0), 300, err);
    CHECK(dup && dup->poll() == MemberClient::JoinState::Rejected && dup->reason() == RejectReason::BadRequest, "duplicate member accepted");
    // 8 groups per local broker
    for (int k = 1; k < 8; ++k) {
        GroupConfig c2;
        c2.sessionKey = "k" + std::to_string(k);
        c2.maxMembers = 2;
        CHECK(net.broker->createGroup(c2, err), "group %d: %s", k, err.c_str());
    }
    GroupConfig c9;
    c9.sessionKey = "nine";
    c9.maxMembers = 2;
    CHECK(!net.broker->createGroup(c9, err), "9th group accepted");
    CHECK(net.broker->discover(g->config().group, "alpha") == g->regionName() && net.broker->discover(g->config().group, "k1").empty(),
          "discovery not keyed");
    GroupConfig noKey;
    CHECK(!LocalBroker([](const std::string&, std::size_t, std::string&) { return std::unique_ptr<SharedRegion>(); }).createGroup(noKey, err),
          "empty session key accepted");
    std::printf("T11 membership: wrong key/group refused, Fs/rail/nominal/manifest/epoch/channels/duplicate rejected with reasons, "
                "full group refused, leave ramps out, 8-group limit, keyed discovery\n");
}

void liveExact(std::uint32_t n, const std::vector<std::uint32_t>& channels, const char* label) {
    Group G;
    if (!makeGroup(G, n, -24.0, kMaxMembers, channels)) return;
    const auto t0 = g_trap;
    const int cycles = 160;
    for (int c = 0; c < cycles; ++c) cycle(G);
    const std::int64_t warm = std::int64_t(G.g->config().latencyQuanta + 24) * kQuantum;   // L + ramps
    const double err = maxExactError(G, warm, G.clock - 256);
    std::uint32_t st = 0;
    double maxBleed = 0;
    bool finite = true;
    for (auto& m : G.members) { st |= m.statusOr; maxBleed = std::max(maxBleed, m.maxAbsBleed); finite = finite && m.finite; }
    const auto d = G.g->diagnostics();
    CHECK(err <= 1e-12, "%s: live bleed error %.3e", label, err);
    CHECK(maxBleed > 0.1 * G.g->gain() * 0.5, "%s: no bleed received (%.3e)", label, maxBleed);
    CHECK(maxBleed <= G.g->gain() * 0.5 + 1e-15, "%s: bleed %.3e exceeds g*max|x|", label, maxBleed);
    CHECK(finite, "%s: non-finite bleed", label);
    CHECK(d.degradedTicks == 0 && d.invalidFrames == 0 && d.resultsDropped == 0, "%s: degraded %lld invalid %lld dropped %lld", label,
          (long long)d.degradedTicks, (long long)d.invalidFrames, (long long)d.resultsDropped);
    CHECK(d.maxRowAbsSum <= G.g->gain() + 1e-18, "%s: row sum %.6g > g %.6g", label, d.maxRowAbsSum, G.g->gain());
    CHECK(!(st & netstatus::kUnavailable) && (st & netstatus::kConnected), "%s: status 0x%x", label, st);
    const long viol = g_trap.violations() - t0.violations();
    CHECK(viol == 0, "%s: %ld RT violations", label, viol);
    std::printf("T11 live %s: %u members, %d quanta, random callback order + partitions {1,17,64,127,128,256}: "
                "bleed error vs reference %.3e, max|bleed| %.3e <= g*0.5 = %.3e, row sum %.4g <= g, %lld ticks, "
                "delay %.3f ms, RT violations %ld\n",
                label, n, cycles, err, maxBleed, G.g->gain() * 0.5, d.maxRowAbsSum, (long long)d.ticks,
                1e3 * G.members[0].client->endpoint().delaySeconds(), viol);
}

void missingCallbacks() {
    Group G;
    if (!makeGroup(G, 4, -30.0)) return;
    const auto t0 = g_trap;
    for (int c = 0; c < 60; ++c) cycle(G);
    std::vector<bool> skip(4, false);
    skip[2] = true;   // host stops calling member 2 for 40 quanta (~107 ms > 50 ms deadline)
    for (int c = 0; c < 40; ++c) cycle(G, &skip);
    const auto mid = G.g->diagnostics();
    for (int c = 0; c < 120; ++c) cycle(G);   // member 2 catches up in a burst, then lockstep again
    const auto d = G.g->diagnostics();
    const double err = maxExactError(G, G.clock - 40 * kQuantum, G.clock - 256);
    bool finite = true;
    double maxBleed = 0;
    for (auto& m : G.members) { finite = finite && m.finite; maxBleed = std::max(maxBleed, m.maxAbsBleed); }
    CHECK(mid.degradedTicks > 0, "no degraded ticks during missing callbacks");
    CHECK((G.members[0].statusOr & netstatus::kDegraded) != 0, "peer did not see degraded status");
    CHECK(d.staleFrames > 0 || d.degradedTicks > 0, "late frames not handled");
    CHECK(d.active == 4, "member evicted by a 107 ms gap (lease is 500 ms)");
    CHECK(err <= 1e-12, "post-recovery bleed error %.3e", err);
    CHECK(finite && maxBleed <= G.g->gain() * 0.5 + 1e-15, "bleed bound/finite during gaps");
    const long viol = g_trap.violations() - t0.violations();
    CHECK(viol == 0, "%ld RT violations with missing callbacks", viol);
    std::printf("T11 missing/reordered callbacks: 40-quantum gap -> %lld degraded ticks (50 ms deadline), %lld stale frames "
                "dropped, peers flagged degraded, exact again after catch-up (error %.3e), RT violations %ld\n",
                (long long)d.degradedTicks, (long long)d.staleFrames, err, viol);
}

void leaseEviction() {
    Group G;
    if (!makeGroup(G, 3, -40.0)) return;
    for (int c = 0; c < 30; ++c) cycle(G);
    const auto gen0 = G.g->diagnostics().rosterGeneration;
    std::vector<bool> skip(3, false);
    skip[1] = true;
    int cyclesToEvict = -1;
    for (int c = 0; c < 260; ++c) {
        cycle(G, &skip);
        if (cyclesToEvict < 0 && G.g->diagnostics().active == 2) cyclesToEvict = c + 1;
    }
    const double ms = cyclesToEvict * kQuantumMs;
    CHECK(cyclesToEvict > 0 && ms >= 500.0 && ms <= 520.0, "eviction after %.1f ms (lease 500 ms)", ms);
    CHECK(G.g->diagnostics().rosterGeneration > gen0, "roster generation not bumped");
    // The evicted member's endpoint falls back (zero bleed, Evicted/Unavailable) on its next call.
    SimMember& m = G.members[1];
    m.statusOr = 0;
    for (int k = 0; k < 40; ++k) runBlock(m, 128);
    CHECK((m.statusOr & netstatus::kEvicted) && (m.statusOr & netstatus::kUnavailable), "evicted endpoint status 0x%x", m.statusOr);
    CHECK(m.bleed[0][std::size_t(m.pos - 1)] == 0.0 && m.client->endpoint().diagnostics().influence < 1.0, "evicted member still coupled");
    // A worker heartbeat keeps a member alive while its audio is suspended.
    Group H;
    if (!makeGroup(H, 2, -40.0)) return;
    std::vector<bool> sk(2, false);
    sk[0] = true;
    for (int c = 0; c < 300; ++c) { H.members[0].client->heartbeat(); cycle(H, &sk); }
    CHECK(H.g->diagnostics().active == 2, "worker heartbeat did not hold the lease");
    std::printf("T11 lease: silent member evicted after %.1f ms (500 ms lease), roster generation bumped, evicted endpoint "
                "falls back to local with zero bleed; worker heartbeat holds a suspended member\n", ms);
}

void brokerDeath(bool graceful) {
    Group G;
    if (!makeGroup(G, 2, -24.0)) return;
    const auto t0 = g_trap;
    for (int c = 0; c < 80; ++c) cycle(G);
    if (graceful) G.g->close();
    int lostAt = -1, zeroAt = -1;
    for (int c = 0; c < 400; ++c) {
        cycle(G, nullptr, /*service=*/false);   // broker crashed: no more service, members keep running
        const auto d = G.members[0].client->endpoint().diagnostics();
        if (lostAt < 0 && d.brokerLost) lostAt = c;
        if (zeroAt < 0 && d.brokerLost && d.influence == 0.0) zeroAt = c;
    }
    const auto d = G.members[0].client->endpoint().diagnostics();
    bool tailZero = true, finite = true;
    for (auto& m : G.members) {
        finite = finite && m.finite;
        for (std::int64_t s = G.clock - 50 * kQuantum; s < G.clock; ++s) tailZero = tailZero && m.bleed[0][std::size_t(s)] == 0.0;
    }
    const long viol = g_trap.violations() - t0.violations();
    const double lostMs = (lostAt + 1) * kQuantumMs;
    if (graceful) CHECK(lostAt == 0, "graceful close not detected at once (%d)", lostAt);
    else CHECK(lostMs >= 495.0 && lostMs <= 515.0, "broker loss detected after %.1f ms", lostMs);
    CHECK(zeroAt >= lostAt && (zeroAt - lostAt) * kQuantumMs <= 50.0 + kQuantumMs, "fade to local took %.1f ms", (zeroAt - lostAt) * kQuantumMs);
    CHECK(d.brokerLost && d.influence == 0.0 && tailZero && finite, "fallback incomplete");
    CHECK((G.members[1].statusOr & netstatus::kBrokerLost) && (G.members[1].statusOr & netstatus::kUnavailable), "status not reported");
    CHECK(viol == 0, "%ld RT violations after broker death", viol);
    std::printf("T11 broker %s mid-run: members never wait (RT violations %ld), loss detected after %.1f ms of audio, "
                "faded to local PSU in %.1f ms, zero bleed, Network unavailable status\n",
                graceful ? "close" : "death", viol, graceful ? 0.0 : lostMs, (zeroAt - lostAt) * kQuantumMs);
}

void fallbacksAndSeek() {
    Group G;
    if (!makeGroup(G, 2, -24.0)) return;
    for (int c = 0; c < 60; ++c) cycle(G);
    SimMember& m = G.members[0];
    // offline / prefetch render, wrong Fs, wrong epoch: unavailable + zero bleed, nothing published
    const auto pub0 = m.client->endpoint().diagnostics().published;
    struct Case { const char* what; bool offline; double fs; std::uint64_t epoch; std::uint32_t flag; };
    const Case cases[] = {{"offline", true, kFs, 0, netstatus::kOffline},
                          {"Fs", false, 44100, 0, netstatus::kRateMismatch},
                          {"epoch", false, kFs, 3, netstatus::kEpochMismatch}};
    for (const auto& c : cases) {
        m.statusOr = 0;
        for (int k = 0; k < 30; ++k) runBlock(m, 128, c.offline, c.fs, c.epoch);
        CHECK((m.statusOr & c.flag) && (m.statusOr & netstatus::kUnavailable), "%s: status 0x%x", c.what, m.statusOr);
        CHECK(m.bleed[0][std::size_t(m.pos - 1)] == 0.0 && m.client->endpoint().diagnostics().influence == 0.0, "%s: not faded", c.what);
    }
    CHECK(m.client->endpoint().diagnostics().published == pub0, "frames published while unavailable");
    // Whole-group seek (loop back): broker resyncs, members restart framing, exact bleed again.
    for (auto& mm : G.members) mm.pos = 4096 + 37;   // unaligned loop point
    G.clock = 4096 + 37;
    for (auto& mm : G.members) mm.statusOr = 0;
    for (int c = 0; c < 120; ++c) cycle(G);
    // after the loop, timeline indices restart; compare the tail of the new pass
    const double err = maxExactError(G, G.clock - 40 * kQuantum, G.clock - 256);
    CHECK(((G.members[0].statusOr | G.members[1].statusOr) & netstatus::kSeek) != 0, "seek not flagged");
    CHECK(G.g->diagnostics().resyncs >= 1, "broker did not resync after group seek");
    CHECK(err <= 1e-12, "bleed after seek error %.3e", err);
    std::printf("T11 fallbacks: offline/prefetch, Fs and epoch mismatch -> Network unavailable, zero bleed, nothing published; "
                "group loop/seek -> %lld broker resync(s), exact bleed again (error %.3e)\n",
                (long long)G.g->diagnostics().resyncs, err);
}

void isolation() {
    Net net;
    std::string err;
    GroupConfig a, b;
    a.sessionKey = "project-A";
    b.sessionKey = "project-B";
    a.maxMembers = b.maxMembers = 2;
    a.bleed = b.bleed = BleedConfig{true, -24.0};
    GroupBroker* ga = net.broker->createGroup(a, err);
    GroupBroker* gb = net.broker->createGroup(b, err);
    CHECK(ga && gb && ga->regionName() != gb->regionName(), "groups not separate");
    if (!ga || !gb) return;
    // Cross-session attach: A's region with B's group/key is refused; discovery is keyed.
    JoinRequest cross = joinFor(*gb, 0);
    CHECK(!MemberClient::attach(net.dir.open(ga->regionName(), err), cross, err), "cross-session attach accepted");
    CHECK(net.broker->discover(ga->config().group, "project-B").empty(), "discovery leaked a foreign region");
    // Two more sessions on ONE local broker, same member UUIDs in both; each receives only its own peers.
    Group GA, GB;
    GB.net = GA.net;
    makeGroup(GA, 2, -24.0, 2);
    makeGroup(GB, 2, -24.0, 2);
    for (auto& m : GB.members) m.idx += 10;   // different signals in session B
    for (int c = 0; c < 120; ++c) { cycle(GA); cycle(GB); }
    const double ea = maxExactError(GA, 40 * kQuantum, GA.clock - 256);
    const double eb = maxExactError(GB, 40 * kQuantum, GB.clock - 256);
    CHECK(ea <= 1e-12 && eb <= 1e-12, "session cross-talk (A %.3e, B %.3e)", ea, eb);
    std::printf("T11 isolation: separate regions per session token, foreign key/group refused, keyed discovery, each session "
                "receives only its own peers (errors %.1e / %.1e)\n", ea, eb);
}

void rowSumBound() {
    // enforceRowSums: random matrices incl. non-finite entries.
    Lcg r{2024};
    double worst = 0;
    int clampedRows = 0;
    for (int t = 0; t < 1000; ++t) {
        TopologyMatrix m{};
        const std::uint32_t n = 2 + r.u() % 31;
        const double g = std::pow(10.0, (-90.0 + 66.0 * (r.u() % 1000) / 999.0) / 20.0);
        for (std::uint32_t i = 0; i < n; ++i)
            for (std::uint32_t j = 0; j < n; ++j) m[i][j] = (double(r.u()) / 4294967296.0 - 0.5) * (r.u() % 4 ? 0.01 : 10.0);
        if (t % 7 == 0) m[0][1] = std::nan("");
        if (t % 11 == 0) m[1][0] = INFINITY;
        if (enforceRowSums(m, n, g)) ++clampedRows;
        for (std::uint32_t i = 0; i < n; ++i) {
            const double s = rowAbsSum(m, i, n);
            CHECK(s <= g && std::isfinite(s), "row %u sum %.17g > g %.17g", i, s, g);
            worst = std::max(worst, s / g);
            CHECK(m[i][i] == 0.0, "diagonal not zero");
        }
    }
    // Live broker with an over-gain topology: clamped + flagged; worst-case aligned input stays <= g.
    Group G;
    GroupConfig cfg;
    cfg.sessionKey = "topo";
    cfg.maxMembers = 4;
    cfg.bleed = BleedConfig{true, -24.0};
    cfg.useTopology = true;
    cfg.topology.assign(4, std::vector<double>(4, 0.5));   // row sum 1.5 >> g
    cfg.topology[2][0] = -0.5;
    std::string err;
    G.g = G.net->broker->createGroup(cfg, err);
    CHECK(G.g && G.g->diagnostics().rowClamped, "over-gain topology not flagged");
    if (!G.g) return;
    for (std::uint32_t i = 0; i < 4; ++i) {
        SimMember m;
        m.idx = i;
        m.client = join(*G.net, *G.g, joinFor(*G.g, i), 0, err);
        CHECK(m.client && m.client->poll() == MemberClient::JoinState::Active, "topo join");
        if (!m.client) return;
        m.bleed.assign(2, {});
        m.partition.assign(1, 128u);
        G.members.push_back(std::move(m));
    }
    for (int c = 0; c < 80; ++c) cycle(G);
    double maxBleed = 0;
    std::uint32_t st = 0;
    for (auto& m : G.members) { maxBleed = std::max(maxBleed, m.maxAbsBleed); st |= m.statusOr; }
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(G.g->effectiveRowAbsSum(i) <= G.g->gain(), "effective row %u > g", i);
    CHECK(maxBleed <= G.g->gain() * 0.5 + 1e-15, "topology bleed %.3e > g*max|x| %.3e", maxBleed, G.g->gain() * 0.5);
    std::printf("T11 cross-talk bound: 1000 random topologies (%d clamped/flagged, incl. NaN/Inf entries) all rows |sum| <= g "
                "(max ratio %.17g); live over-gain matrix clamped+flagged, max|bleed| %.3e <= g*max|x| %.3e\n",
                clampedRows, worst, maxBleed, G.g->gain() * 0.5);
}

}  // namespace

int main() {
    trapPositiveControls();
    membershipAndCompatibility();
    liveExact(2, {2}, "2 stereo");
    liveExact(32, {2, 1}, "32 mixed mono/stereo");
    missingCallbacks();
    leaseEviction();
    brokerDeath(false);
    brokerDeath(true);
    fallbacksAndSeek();
    isolation();
    rowSumBound();
    std::printf("T11 live total RT trap: allocs %ld frees %ld locks %ld condWaits %ld io %ld%s\n", g_trap.allocations,
                g_trap.deallocations, g_trap.mutexLocks, g_trap.condWaits, g_trap.ioCalls,
                cd::test::kRtTrapLockIoInstrumented ? "" : " [lock/IO NOT INSTRUMENTED]");
    if (g_fail) { std::printf("network_live: %d failure(s)\n", g_fail); return 1; }
    std::printf("network_live: all checks passed\n");
    return 0;
}
