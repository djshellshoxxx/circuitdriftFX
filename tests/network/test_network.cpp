// cd_network tests (T11 worker-side parts). Synthetic fixtures; G05 host/process matrix stays open.
#include <atomic>
#include <chrono>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <numbers>
#include <random>
#include <thread>
#include "network/InfluenceBundle.h"
#include "network/InstanceNetwork.h"
#include "network/SharedMemory.h"
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace cd::network;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

constexpr double kFs = 48000.0;
GroupSpec specFor(std::uint64_t id, std::uint64_t session = 1) {
    GroupSpec s; s.groupId = {0x1111, id}; s.sessionKeyHash = session; s.sampleRate = kFs; s.railModelId = 7; s.nominalRailV = 300.0f; s.bleedSeed = 99;
    return s;
}
Uuid128 mid(std::uint64_t i) { return {0xABCD, i + 1}; }

std::vector<std::unique_ptr<Member>> joinN(Registry& reg, const GroupSpec& s, unsigned n) {
    std::vector<std::unique_ptr<Member>> m;
    for (unsigned i = 0; i < n; ++i) {
        std::unique_ptr<Member> p; std::string why;
        const auto r = reg.join(s, mid(i), p, &why);
        CHECK(r == JoinResult::Ok);
        m.push_back(std::move(p));
    }
    return m;
}

struct Audio { float l[kQuantumFrames], r[kQuantumFrames]; };
void fillAudio(Audio& a, float v) { for (unsigned i = 0; i < kQuantumFrames; ++i) a.l[i] = a.r[i] = v; }

// One tick for all members in the given order; returns last ConsumeOut per member.
struct Run {
    std::vector<ConsumeOut> out;
    std::vector<std::vector<float>> bleed;
};
Run tick(std::vector<std::unique_ptr<Member>>& ms, std::uint64_t k, const std::vector<std::size_t>& order, const std::vector<float>& demand, float audioLevel) {
    Run r; r.out.resize(ms.size()); r.bleed.assign(ms.size(), std::vector<float>(kQuantumFrames * 2, 0.f));
    Audio a; fillAudio(a, audioLevel);
    for (auto i : order) {
        MemberFrameIn in; in.demandA = demand[i]; in.powerW = demand[i] * 300.f; in.activity = 0.5f; in.left = a.l; in.right = a.r;
        CHECK(ms[i]->publish(k, in));
        ms[i]->consume(k, r.bleed[i].data(), r.bleed[i].data() + kQuantumFrames, r.out[i]);
    }
    return r;
}
std::vector<std::size_t> iota(std::size_t n) { std::vector<std::size_t> o(n); for (std::size_t i = 0; i < n; ++i) o[i] = i; return o; }

void testNoexceptContract() {
    Member* m = nullptr; MemberFrameIn in; ConsumeOut o;
    static_assert(noexcept(m->publish(0, in)) && noexcept(m->consume(0, nullptr, nullptr, o)) && noexcept(m->removed()));
    static_assert(noexcept(m->setBleed(true, -40)) && noexcept(m->setNetworkAvailable(true)));
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);
}

void testJoinRules() {
    Registry reg;
    auto spec = specFor(1);
    auto two = joinN(reg, spec, 2);
    CHECK(reg.memberCount(spec) == 2 && reg.groupCount() == 1);
    std::unique_ptr<Member> p; std::string why;
    CHECK(reg.join(spec, mid(0), p, &why) == JoinResult::Duplicate && !why.empty());
    auto bad = spec; bad.sampleRate = 44100;   // differs from the group's creator? same key => same group spec; creator wins
    // A different sample rate with the same group id/session is rejected by the existing group header.
    CHECK(reg.join(bad, mid(5), p, &why) == JoinResult::IncompatibleSampleRate);
    bad = spec; bad.railModelId = 8; CHECK(reg.join(bad, mid(5), p, &why) == JoinResult::IncompatibleRail);
    bad = spec; bad.nominalRailV = 12.0f; CHECK(reg.join(bad, mid(5), p, &why) == JoinResult::IncompatibleRail);
    CHECK(reg.join(spec, Uuid128{0, 0}, p, &why) == JoinResult::InvalidArgument);
    auto none = spec; none.groupId = {0, 0}; CHECK(reg.join(none, mid(5), p, &why) == JoinResult::InvalidArgument);

    // 32 members OK, 33rd refused.
    Registry big; auto s32 = specFor(2);
    auto m32 = joinN(big, s32, 32);
    CHECK(big.join(s32, mid(40), p, &why) == JoinResult::MemberLimit);
    // 8 groups per broker.
    Registry g; std::vector<std::unique_ptr<Member>> keep;
    for (unsigned i = 0; i < 8; ++i) { std::unique_ptr<Member> m; CHECK(g.join(specFor(100 + i), mid(0), m, &why) == JoinResult::Ok); keep.push_back(std::move(m)); }
    CHECK(g.join(specFor(200), mid(0), p, &why) == JoinResult::GroupLimit);
    // Freed groups are recycled; old members stay memory safe.
    g.leave(*keep[0]);
    CHECK(g.join(specFor(200), mid(0), p, &why) == JoinResult::Ok);
    CHECK(keep[0]->removed() && !keep[0]->publish(0, MemberFrameIn{}));
}

void testIsolation() {
    Registry reg;
    auto a = joinN(reg, specFor(3, /*session*/ 1), 2);
    auto b = joinN(reg, specFor(3, /*session*/ 2), 2);  // same group UUID, other session: isolated
    CHECK(reg.groupCount() == 2);
    std::vector<float> dA = {1.f, 1.f}, dB = {50.f, 50.f};
    Run ra, rb;
    for (std::uint64_t k = 0; k < 80; ++k) { ra = tick(a, k, iota(2), dA, 0.5f); rb = tick(b, k, iota(2), dB, 0.5f); }
    CHECK(std::abs(ra.out[0].sharedDemandA - 2.0f) < 1e-4f);
    CHECK(std::abs(rb.out[0].sharedDemandA - 100.0f) < 1e-3f);
    CHECK(ra.out[0].members == 2 && rb.out[0].members == 2);
}

void testAggregationOrderIndependence(unsigned n) {
    for (int variant = 0; variant < 3; ++variant) {
        Registry reg; auto spec = specFor(10 + n);
        auto ms = joinN(reg, spec, n);
        for (auto& m : ms) m->setBleed(true, -30.0);
        std::vector<float> demand(n); float sum = 0;
        for (unsigned i = 0; i < n; ++i) { demand[i] = 1.0f + 0.1f * float(i); sum += demand[i]; }
        std::mt19937 rng(1234 + unsigned(variant));
        Run last;
        std::vector<float> trace;
        for (std::uint64_t k = 0; k < 120; ++k) {
            auto order = iota(n);
            if (variant > 0) std::shuffle(order.begin(), order.end(), rng);
            last = tick(ms, k, order, demand, 0.25f);
            trace.push_back(last.out[0].sharedDemandA);
            for (auto& o : last.out) CHECK(std::isfinite(o.sharedDemandA));
        }
        for (unsigned i = 0; i < n; ++i) CHECK(std::abs(last.out[i].sharedDemandA - sum) < 1e-3f * sum);
        CHECK(last.out[0].status == MemberStatus::Coupled && last.out[0].missingPeers == 0);
        static std::vector<float> reference;
        if (n == 2 || n == 32) { if (variant == 0) reference = trace; else CHECK(trace == reference); }  // callback order does not change results
    }
}

void testRowSumBound() {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-3.f, 3.f);
    Uuid128 ids[kMaxMembers];
    for (unsigned i = 0; i < kMaxMembers; ++i) ids[i] = mid(i);
    for (double dB : {-90.0, -60.0, -40.0, -24.0, -10.0 /* clamped to -24 */}) {
        const double g = bleedGainLinear(true, dB);
        CHECK(g <= std::pow(10.0, -24.0 / 20.0) + 1e-12);
        for (unsigned n = 2; n <= kMaxMembers; ++n) {
            const std::uint32_t mask = n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
            for (unsigned self = 0; self < n; ++self) {
                float w[kMaxMembers], row[kMaxMembers];
                const double s1 = bleedRowWeights(self, ids, mask, 77, g, nullptr, w);
                CHECK(s1 <= g * (1 + 1e-9) && w[self] == 0.f);
                for (auto& r : row) r = u(rng);  // hostile topology far above g
                const double s2 = bleedRowWeights(self, ids, mask, 77, g, row, w);
                double chk = 0; for (float x : w) chk += std::abs(double(x));
                CHECK(s2 <= g * (1 + 1e-6) && chk <= g * (1 + 1e-6));
                row[0] = std::nanf("");
                CHECK(bleedRowWeights(self, ids, mask, 77, g, row, w) <= g * (1 + 1e-6));
            }
        }
    }
    CHECK(bleedGainLinear(false, -40) == 0.0);
    // Through the real audio path with hostile topology and full-scale audio.
    Registry reg; auto ms = joinN(reg, specFor(30), 8);
    for (auto& m : ms) { m->setBleed(true, -24.0); float row[kMaxMembers]; for (auto& r : row) r = 5.f; CHECK(m->setTopologyRow(row)); }
    std::vector<float> d(8, 1.f); Run r;
    float peak = 0;
    for (std::uint64_t k = 0; k < 100; ++k) { r = tick(ms, k, iota(8), d, 1.0f); for (float v : r.bleed[0]) peak = std::max(peak, std::abs(v)); CHECK(r.out[0].bleedRowAbsSum <= bleedGainLinear(true, -24) * 1.0001); }
    CHECK(peak <= bleedGainLinear(true, -24) * 1.0001 && peak > 0);
    // Bleed off => exactly zero.
    for (auto& m : ms) m->setBleed(false, -40);
    r = tick(ms, 100, iota(8), d, 1.0f);
    for (float v : r.bleed[0]) CHECK(v == 0.f);
    // Mono duplicates at -3 dB.
    Registry r2; auto two = joinN(r2, specFor(31), 2);
    for (auto& m : two) m->setBleed(true, -24.0);
    float mono[kQuantumFrames]; for (auto& v : mono) v = 1.f;
    float bl[kQuantumFrames], br[kQuantumFrames]; ConsumeOut o;
    for (std::uint64_t k = 0; k < 60; ++k) {
        MemberFrameIn in; in.demandA = 1; in.left = mono; in.right = nullptr;
        two[1]->publish(k, in); two[0]->publish(k, in);
        two[0]->consume(k, bl, br, o);
    }
    const float expect = float(bleedGainLinear(true, -24.0) * std::pow(10.0, -3.0 / 20.0));
    CHECK(std::abs(std::abs(bl[5]) - expect) < 1e-5f && std::abs(bl[5] - br[5]) < 1e-7f);
}

void testMissingLateAndDecay() {
    Registry reg; auto ms = joinN(reg, specFor(40), 2);
    std::vector<float> d = {2.f, 4.f};
    Run r; std::uint64_t k = 0;
    for (; k < 60; ++k) r = tick(ms, k, iota(2), d, 0.1f);
    CHECK(std::abs(r.out[0].sharedDemandA - 6.f) < 1e-3f);
    // Peer 1 stops publishing (late/missing); peer 0 never waits, holds and decays with tau 100 ms.
    const std::uint64_t lastPublished = k - 1;
    const double decay = std::exp(-128.0 / (kFs * 0.1));
    Audio a; fillAudio(a, 0.1f); std::vector<float> bl(256);
    for (int step = 0; step < 100; ++step, ++k) {
        MemberFrameIn in; in.demandA = 2.f; in.left = a.l; in.right = a.r;
        ms[0]->publish(k, in);
        ConsumeOut o; ms[0]->consume(k, bl.data(), bl.data() + 128, o);
        const std::uint64_t t = k - kLiveLatencyQuanta;
        if (t > lastPublished) {
            const double m = double(t - lastPublished);
            CHECK(o.status == MemberStatus::Degraded && o.missingPeers == 1);
            CHECK(std::abs(double(o.sharedDemandA) - (2.0 + 4.0 * std::pow(decay, m))) < 1e-3);
            for (float v : bl) CHECK(v == 0.f);  // no audio from a missing frame
        }
    }
    // Late frames beyond the ring depth are never read (stale tick stamp) and 64-tick-old data is not reused.
    ConsumeOut o; ms[0]->consume(k + 200, bl.data(), bl.data() + 128, o);
    CHECK(o.missingPeers == 1);
    // Delayed-by-3-ticks peer still within L=8 is fully coupled; delayed by 12 is degraded.
    for (int delay : {3, 12}) {
        Registry rg; auto m2 = joinN(rg, specFor(41 + std::uint64_t(delay)), 2);
        ConsumeOut last; std::vector<float> b(256);
        for (std::uint64_t t = 0; t < 120; ++t) {
            MemberFrameIn in; in.demandA = 3.f;
            m2[0]->publish(t, in);
            if (t >= std::uint64_t(delay)) m2[1]->publish(t - std::uint64_t(delay), in);
            m2[0]->consume(t, b.data(), b.data() + 128, last);
        }
        CHECK((last.status == MemberStatus::Coupled) == (delay == 3));
    }
    // Epoch change (seek/loop): frames of the old epoch are rejected, then a matching epoch recovers.
    Registry re; auto m3 = joinN(re, specFor(50), 2); std::vector<float> dd = {1.f, 1.f};
    Run rr; for (std::uint64_t t = 0; t < 40; ++t) rr = tick(m3, t, iota(2), dd, 0.f);
    m3[0]->setEpoch(5);
    rr = tick(m3, 40, iota(2), dd, 0.f);
    CHECK(rr.out[0].status == MemberStatus::Degraded);
    m3[1]->setEpoch(5);
    for (std::uint64_t t = 41; t < 70; ++t) rr = tick(m3, t, iota(2), dd, 0.f);
    CHECK(rr.out[0].status == MemberStatus::Coupled);
}

void testDeathAndBroker() {
    Registry reg; auto spec = specFor(60);
    auto ms = joinN(reg, spec, 3);
    std::vector<float> d = {1.f, 2.f, 3.f}; Run r; std::uint64_t k = 0;
    CHECK(!ms[0]->removed());
    for (; k < 60; ++k) r = tick(ms, k, iota(3), d, 0.3f);
    const std::int64_t t0 = 1'000'000'000;
    reg.reapStale(t0);  // baseline: records every member's current heartbeat
    // Member 2 dies (stops publishing); 0 and 1 keep their heartbeats advancing.
    const std::vector<std::size_t> alive = {0, 1};
    tick(ms, k++, alive, d, 0.3f); CHECK(reg.reapStale(t0 + 100'000'000) == 0);
    tick(ms, k++, alive, d, 0.3f); CHECK(reg.reapStale(t0 + 499'000'000) == 0);   // 499 ms: still a member
    tick(ms, k++, alive, d, 0.3f); CHECK(reg.reapStale(t0 + 500'000'000) == 1);   // 500 ms: removed by the worker
    CHECK(ms[2]->removed() && !ms[2]->publish(k, MemberFrameIn{}));
    CHECK(reg.memberCount(spec) == 2);
    // Survivors fade the dead member out over >= 50 ms and report a 2-member roster.
    for (int i = 0; i < 80; ++i, ++k) r = tick(ms, k, alive, d, 0.3f);
    CHECK(r.out[0].members == 2 && std::abs(r.out[0].sharedDemandA - 3.f) < 1e-3f && r.out[0].status == MemberStatus::Coupled);
    // A removed member's consume is safe and reports Removed.
    std::vector<float> b(256); ConsumeOut o; ms[2]->consume(k, b.data(), b.data() + 128, o);
    CHECK(o.status == MemberStatus::Removed);
    // Rejoin gets a fresh generation, stale frames cannot leak back.
    std::unique_ptr<Member> again; CHECK(reg.join(spec, mid(2), again) == JoinResult::Ok);
    CHECK(!again->removed());

    // Broker death: heartbeats/reaping stop, members keep running; fade to local PSU on unavailable.
    Registry reg2; auto spec2 = specFor(61); auto m2 = joinN(reg2, spec2, 2);
    for (auto& m : m2) m->setBleed(true, -30.0);
    std::vector<float> d2 = {2.f, 5.f}; Run rr; std::uint64_t t = 0;
    for (; t < 60; ++t) rr = tick(m2, t, iota(2), d2, 0.5f);
    CHECK(std::abs(rr.out[0].sharedDemandA - 7.f) < 1e-3f);
    for (auto& m : m2) m->setNetworkAvailable(false);
    int quanta = 0;
    for (; quanta < 100; ++quanta, ++t) { rr = tick(m2, t, iota(2), d2, 0.5f); if (rr.out[0].coupling == 0.f) break; }
    const int expectQuanta = int(std::ceil(0.050 * kFs / 128.0));
    CHECK(quanta >= expectQuanta - 1 && quanta <= expectQuanta + 1);     // 50 ms ramp
    CHECK(rr.out[0].status == MemberStatus::Unavailable && std::abs(rr.out[0].sharedDemandA - 2.f) < 1e-6f);
    for (float v : rr.bleed[0]) CHECK(v == 0.f);                        // zero bleed, local PSU only
    for (auto& m : m2) m->setNetworkAvailable(true);                     // recovery
    for (int i = 0; i < 60; ++i, ++t) rr = tick(m2, t, iota(2), d2, 0.5f);
    CHECK(std::abs(rr.out[0].sharedDemandA - 7.f) < 1e-3f);
}

void testHostilePeer() {
    // A hostile writer scribbles NaN/Inf/huge values and random stamps while a member consumes.
    auto spec = specFor(70);
    SharedLayout* L = nullptr;
    auto layout = std::make_unique<SharedLayout>(); fillHeader(*layout, spec); L = layout.get();
    Group grp(L); std::unique_ptr<Member> a, b; std::string why;
    CHECK(grp.join(mid(0), kFs, 7, 300.f, a, &why) == JoinResult::Ok && grp.join(mid(1), kFs, 7, 300.f, b, &why) == JoinResult::Ok);
    a->setBleed(true, -24.0);
    std::atomic<bool> stop{false};
    std::thread hostile([&] {
        std::mt19937 rng(9);
        while (!stop.load()) {
            FrameSlot& f = L->members[1].ring[rng() % kRingQuanta];
            f.stamp.store(rng() % 200 + 1, std::memory_order_relaxed);
            f.epoch.store(0, std::memory_order_relaxed); f.generation.store(b->slot() == 1 ? 1u : 0u, std::memory_order_relaxed);
            const std::uint32_t bad[] = {0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x7F7FFFFFu, 0x00000001u, 0xDEADBEEFu};
            f.demandBits.store(bad[rng() % 6], std::memory_order_relaxed);
            f.powerBits.store(bad[rng() % 6], std::memory_order_relaxed);
            f.hasAudio.store(1, std::memory_order_relaxed);
            for (auto& x : f.audio) x.store(bad[rng() % 6], std::memory_order_relaxed);
        }
    });
    float bl[kQuantumFrames], br[kQuantumFrames]; ConsumeOut o; bool finite = true;
    for (std::uint64_t k = 0; k < 20000; ++k) {
        MemberFrameIn in; in.demandA = 1.f; a->publish(k, in);
        a->consume(k, bl, br, o);
        finite = finite && std::isfinite(o.sharedDemandA) && o.sharedDemandA <= 1.0e4f * 2 + 1;
        for (unsigned i = 0; i < kQuantumFrames; ++i) finite = finite && std::isfinite(bl[i]) && std::isfinite(br[i]);
    }
    stop = true; hostile.join();
    CHECK(finite);
}

void testConcurrentNoWaits() {
    // Real threads, 8 members, each audio thread runs its own loop at full speed: no waits by design;
    // the test asserts completion, finiteness and that the shared demand stays within bounds.
    Registry reg; auto spec = specFor(80); const unsigned n = 8;
    auto ms = joinN(reg, spec, n);
    std::atomic<int> bad{0}; std::atomic<std::int64_t> worstNs{0};
    std::vector<std::thread> th;
    for (unsigned i = 0; i < n; ++i) th.emplace_back([&, i] {
        Audio a; fillAudio(a, 0.2f); std::vector<float> b(256); ConsumeOut o;
        for (std::uint64_t k = 0; k < 3000; ++k) {
            MemberFrameIn in; in.demandA = 1.f; in.left = a.l; in.right = a.r;
            const auto t0 = std::chrono::steady_clock::now();
            ms[i]->publish(k, in);
            ms[i]->consume(k, b.data(), b.data() + 128, o);
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            auto w = worstNs.load(); while (ns > w && !worstNs.compare_exchange_weak(w, ns)) {}
            if (!std::isfinite(o.sharedDemandA) || o.sharedDemandA > float(n) + 0.01f) ++bad;
        }
    });
    for (auto& t : th) t.join();
    CHECK(bad == 0);
    std::printf("  concurrent: 8 members x 3000 ticks, worst publish+consume %lld us (informational)\n", static_cast<long long>(worstNs.load() / 1000));
}

void testValidateLayout() {
    GroupSpec s = specFor(90);
    auto lay = std::make_unique<SharedLayout>();
    fillHeader(*lay, s);
    std::string err;
    CHECK(validateLayout(lay.get(), sizeof(SharedLayout), err));
    auto mutate = [&](auto fn, const char* what) {
        auto c = std::make_unique<SharedLayout>(); fillHeader(*c, s); fn(c->header);
        std::string e; const bool ok = validateLayout(c.get(), sizeof(SharedLayout), e);
        if (ok) { ++failures; std::printf("FAIL accepted corrupt header: %s\n", what); }
    };
    mutate([](SharedHeader& h) { h.magic ^= 1; }, "magic");
    mutate([](SharedHeader& h) { h.major = 2; }, "major");
    mutate([](SharedHeader& h) { h.byteSize = 4; }, "byteSize small");
    mutate([](SharedHeader& h) { h.byteSize = sizeof(SharedLayout) * 2; }, "byteSize large");
    mutate([](SharedHeader& h) { h.slotCount = 1000; }, "slotCount");
    mutate([](SharedHeader& h) { h.ringQuanta = 0; }, "ringQuanta");
    mutate([](SharedHeader& h) { h.quantumFrames = 64; }, "quantum");
    mutate([](SharedHeader& h) { h.channels = 9; }, "channels");
    mutate([](SharedHeader& h) { h.sampleRate = std::nan(""); }, "NaN rate");
    mutate([](SharedHeader& h) { h.sampleRate = 10; }, "rate low");
    mutate([](SharedHeader& h) { h.nominalRailV = INFINITY; }, "rail inf");
    // A producer must not create a region that the receiver's structural
    // admission will reject. Bounds are inclusive and symmetric.
    auto checkInitRail = [&](float rail, bool expected) {
        auto target = std::make_unique<SharedLayout>();
        auto candidate = s;
        candidate.nominalRailV = rail;
        std::string e;
        auto* initialized = initializeLayout(target.get(), sizeof(SharedLayout), candidate, e);
        CHECK((initialized != nullptr) == expected);
        if (initialized) CHECK(validateLayout(initialized, sizeof(SharedLayout), e));
    };
    checkInitRail(10000.0f, true);
    checkInitRail(-10000.0f, true);
    checkInitRail(10000.125f, false);
    checkInitRail(-10000.125f, false);
    CHECK(!validateLayout(lay.get(), sizeof(SharedHeader) - 1, err));
    CHECK(!validateLayout(lay.get(), sizeof(SharedLayout) - 1, err));
    CHECK(!validateLayout(nullptr, 100, err));
    CHECK(!validateLayout(reinterpret_cast<const char*>(lay.get()) + 1, sizeof(SharedLayout), err));
}

#if !defined(_WIN32)
void testSharedMemory() {
    const std::uint64_t unique = std::uint64_t(::getpid()) * 7919u + std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    GroupSpec spec = specFor(unique, unique ^ 0x55);
    std::string err;
    auto region = SharedMemoryRegion::create(spec, err);
    if (!region) { std::printf("  shared memory unavailable (%s): skipping cross-process tests\n", err.c_str()); return; }
    CHECK(region->size() == sizeof(SharedLayout));
    CHECK(!SharedMemoryRegion::create(spec, err));                       // exclusive
    CHECK(!SharedMemoryRegion::openByName("/../etc/passwd", err));        // name safety
    CHECK(!SharedMemoryRegion::openByName("/cdip-ZZ", err));
    CHECK(!SharedMemoryRegion::open(spec.groupId, spec.sessionKeyHash + 1, err));  // other session: no such object
    Group broker(region->layout());
    std::unique_ptr<Member> parent; CHECK(broker.join(mid(0), kFs, 7, 300.f, parent, &err) == JoinResult::Ok);
    parent->setBleed(true, -30);

    const pid_t pid = fork();
    if (pid == 0) {  // member in another process: attaches, joins, publishes, then dies without leaving
        std::string e;
        auto r = SharedMemoryRegion::open(spec.groupId, spec.sessionKeyHash, e);
        if (!r) _exit(2);
        Group g(r->layout());
        std::unique_ptr<Member> m;
        if (g.join(mid(1), kFs, 7, 300.f, m, &e) != JoinResult::Ok) _exit(3);
        std::vector<float> a(kQuantumFrames, 0.5f);
        for (std::uint64_t k = 0; k < 40; ++k) { MemberFrameIn in; in.demandA = 2.f; in.left = a.data(); in.right = a.data(); if (!m->publish(k, in)) _exit(4); }
        _exit(0);
    }
    int status = 0; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::vector<float> a(kQuantumFrames, 0.5f), bl(kQuantumFrames), br(kQuantumFrames); ConsumeOut o;
    for (std::uint64_t k = 0; k < 40; ++k) { MemberFrameIn in; in.demandA = 1.f; in.left = a.data(); in.right = a.data(); parent->publish(k, in); parent->consume(k, bl.data(), bl.data() + 0, o); }
    float b2[kQuantumFrames];
    parent->consume(39, bl.data(), b2, o);
    CHECK(o.members == 2 && std::abs(o.sharedDemandA - 3.f) < 1e-3f);    // child's frames crossed the process boundary
    CHECK(std::abs(bl[0]) > 0.f);
    // Child died: broker reaps it after 500 ms without heartbeat.
    std::int64_t now = 5'000'000'000;
    broker.reapStale(now);  // baseline
    { MemberFrameIn in; in.demandA = 1.f; parent->publish(40, in); }
    CHECK(broker.reapStale(now + 499'000'000) == 0);
    { MemberFrameIn in; in.demandA = 1.f; parent->publish(41, in); }
    CHECK(broker.reapStale(now + 500'000'000) == 1 && broker.memberCount() == 1);

    // Corrupt header in the live region: a new attach is rejected.
    region->layout()->header.magic = 0;
    CHECK(!SharedMemoryRegion::open(spec.groupId, spec.sessionKeyHash, err));
    region->layout()->header.magic = kMagic;
    region->layout()->header.byteSize = 12;
    CHECK(!SharedMemoryRegion::open(spec.groupId, spec.sessionKeyHash, err));
    region->layout()->header.byteSize = sizeof(SharedLayout);
    auto ok = SharedMemoryRegion::open(spec.groupId, spec.sessionKeyHash, err);
    CHECK(ok != nullptr);
}
#else
void testSharedMemory() { std::printf("  shared memory tests run on POSIX only in this suite\n"); }
#endif

// ---- P16 ----
void testBundle() {
    for (unsigned n : {2u, 5u, 32u}) {
        std::mt19937 rng(77 + n);
        std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        const std::uint64_t len = 128 * 40 + 37;  // not a multiple of the quantum
        std::vector<MemberStem> stems(n);
        for (unsigned i = 0; i < n; ++i) {
            stems[i].id = mid(n - i);  // deliberately unsorted
            stems[i].left.resize(len); stems[i].right.resize(len);
            for (std::uint64_t s = 0; s < len; ++s) { stems[i].left[s] = u(rng); stems[i].right[s] = u(rng); }
            stems[i].contentHash = hashSamples(stems[i].left.data(), len) ^ hashSamples(stems[i].right.data(), len);
        }
        CoRenderConfig cfg; cfg.lengthSamples = len; cfg.startSample = 4096; cfg.bleedSeed = 3; cfg.bleedDb = -30; cfg.latencyQuanta = 4;
        // Rail-dependent nonlinear demand (contractive): needs the joint fixed-point solve.
        DemandFn dem = [n](std::size_t m, std::uint64_t k, double v) { return 0.02 * double(m + 1) / double(n) * (1.0 + 0.5 * std::sin(0.1 * double(k))) * (300.0 / v) * 0.5 + 0.001 * v / 300.0; };
        InfluenceBundle ref; std::string err;
        CHECK(coRender(cfg, stems, dem, ref, err));
        if (!err.empty()) std::printf("  coRender: %s\n", err.c_str());
        // Row-sum bound for the bundle's bleed: |c| <= g * max|x| for every receiver/sample.
        const double g = bleedGainLinear(true, -30);
        float peak = 0; for (float v : ref.bleed) peak = std::max(peak, std::abs(v));
        CHECK(peak <= g * 0.5 * 1.0001 && peak > 0);

        // Reference "plugin renders" (member order A, block 128) vs replays in other orders/partitions.
        auto render = [&](const InfluenceBundle& b, const std::vector<std::size_t>& order, const std::vector<std::size_t>& blocks) {
            BundleReplayer rp; std::string e;
            CHECK(rp.open(b, ref.manifest, e));
            std::vector<std::vector<float>> y(n, std::vector<float>(len * 2));
            // Sorted-id index of each stem
            std::vector<std::size_t> sortedIdx(n); for (unsigned i = 0; i < n; ++i) sortedIdx[i] = i;
            std::sort(sortedIdx.begin(), sortedIdx.end(), [&](std::size_t a, std::size_t c) { return stems[a].id < stems[c].id; });
            std::vector<std::size_t> posOfStem(n); for (unsigned p = 0; p < n; ++p) posOfStem[sortedIdx[p]] = p;
            std::vector<std::uint64_t> cursor(n, 0);
            std::size_t bi = 0; bool progress = true;
            while (progress) {
                progress = false;
                for (auto i : order) {
                    if (cursor[i] >= len) continue;
                    const std::size_t blk = blocks[bi++ % blocks.size()];
                    const std::size_t cnt = std::size_t(std::min<std::uint64_t>(blk, len - cursor[i]));
                    std::vector<float> bl(cnt), br(cnt), volts(cnt), amps(cnt);
                    if (!rp.readBleed(posOfStem[i], cfg.startSample + cursor[i], cnt, bl.data(), br.data()) ||
                        !rp.readRail(cfg.startSample + cursor[i], cnt, volts.data(), amps.data())) { CHECK(false); return y; }
                    for (std::size_t s = 0; s < cnt; ++s) {
                        const std::size_t at = std::size_t(cursor[i]) + s;
                        y[i][at * 2] = stems[i].left[at] * (volts[s] / 300.f) + bl[s];
                        y[i][at * 2 + 1] = stems[i].right[at] * (volts[s] / 300.f) + br[s];
                    }
                    cursor[i] += cnt; progress = true;
                }
            }
            return y;
        };
        auto base = render(ref, iota(n), {128});
        double worst = 0;
        std::mt19937 r2(n);
        for (auto blocks : {std::vector<std::size_t>{1}, {37, 211, 64}, {1000}, {128, 7}}) {
            auto order = iota(n); std::shuffle(order.begin(), order.end(), r2);
            auto y = render(ref, order, blocks);
            for (unsigned i = 0; i < n; ++i) for (std::size_t s = 0; s < y[i].size(); ++s) worst = std::max(worst, double(std::abs(y[i][s] - base[i][s])));
        }
        CHECK(worst <= 1e-6);

        // Serialisation round trip reproduces the identical bundle; replay from parsed data is exact.
        const auto bytes = serializeBundle(ref);
        InfluenceBundle back;
        CHECK(parseBundle(bytes.data(), bytes.size(), back, err));
        CHECK(back.manifest == ref.manifest && back.bleed == ref.bleed && back.railVolts == ref.railVolts);
        CHECK(serializeBundle(back) == bytes);
        if (n == 2) {
            // Corruption: any flipped byte, truncation, and a forged count with a recomputed hash are rejected.
            for (std::size_t at : {std::size_t(0), std::size_t(20), bytes.size() / 2, bytes.size() - 1}) {
                auto bad = bytes; bad[at] ^= 0x40; InfluenceBundle x; CHECK(!parseBundle(bad.data(), bad.size(), x, err));
            }
            for (std::size_t cut : {std::size_t(0), std::size_t(15), bytes.size() - 1}) { InfluenceBundle x; CHECK(!parseBundle(bytes.data(), cut, x, err)); }
            auto forged = bytes;
            // member count lives after magic,version,fs,start,len,latency,seed,3 doubles,strlen,string:
            const std::size_t countAt = 4 + 4 + 8 + 8 + 8 + 4 + 8 + 24 + 4 + cfg.modelVersion.size();
            forged[countAt] = 33;
            const std::uint64_t h = fnv1a64(forged.data(), forged.size() - 8);
            for (int i = 0; i < 8; ++i) forged[forged.size() - 8 + std::size_t(i)] = std::uint8_t(h >> (8 * i));
            InfluenceBundle x; CHECK(!parseBundle(forged.data(), forged.size(), x, err));
            // Manifest mismatches invalidate replay.
            BundleReplayer rp; auto m = ref.manifest;
            m.inputHashes[0] ^= 1; CHECK(!rp.open(ref, m, err) && err.find("hash") != std::string::npos);
            m = ref.manifest; m.startSample += 1; CHECK(!rp.open(ref, m, err));
            m = ref.manifest; m.sampleRate = 44100; CHECK(!rp.open(ref, m, err));
            m = ref.manifest; std::swap(m.memberIds[0], m.memberIds[1]); CHECK(!rp.open(ref, m, err));
            m = ref.manifest; m.modelVersion = "x"; CHECK(!rp.open(ref, m, err));
            CHECK(rp.open(ref, ref.manifest, err));
            float a1[8], a2[8];
            CHECK(!rp.readBleed(0, cfg.startSample + len - 4, 8, a1, a2));  // past the end: refused
            CHECK(!rp.readBleed(0, cfg.startSample - 1, 1, a1, a2));
            CHECK(!rp.readBleed(5, cfg.startSample, 1, a1, a2));
        }
    }
    // Unconverged joint solve is rejected; non-finite too.
    std::vector<MemberStem> st(2);
    for (int i = 0; i < 2; ++i) { st[std::size_t(i)].id = mid(std::uint64_t(i)); st[std::size_t(i)].left.assign(256, 0); st[std::size_t(i)].right.assign(256, 0); }
    CoRenderConfig cfg; cfg.lengthSamples = 256; InfluenceBundle b; std::string err;
    CHECK(!coRender(cfg, st, [](std::size_t, std::uint64_t, double v) { return 0.05 * v; }, b, err) && err.find("converge") != std::string::npos);
    CHECK(!coRender(cfg, st, [](std::size_t, std::uint64_t, double) { return std::nan(""); }, b, err));
    CHECK(coRender(cfg, st, [](std::size_t, std::uint64_t, double) { return 0.1; }, b, err));
    st[1].id = st[0].id; CHECK(!coRender(cfg, st, [](std::size_t, std::uint64_t, double) { return 0.1; }, b, err));
    // Known FNV-1a vector.
    CHECK(fnv1a64("a", 1) == 0xAF63DC4C8601EC8Cull);
}

void testBundleValidation() {
    InfluenceBundle valid;
    valid.manifest.lengthSamples = kQuantumFrames;
    valid.manifest.modelVersion = "0.0.1-dev";
    valid.manifest.memberIds = {mid(1), mid(2)};
    valid.manifest.inputHashes = {1, 2};
    valid.railVolts = {300.f};
    valid.railDemandA = {0.1f};
    valid.bleed.assign(2 * kAudioChannels * kQuantumFrames, 0.f);
    const auto bytes = serializeBundle(valid);
    CHECK(!bytes.empty());
    const std::size_t countAt = 72 + valid.manifest.modelVersion.size();
    const std::size_t payloadAt = countAt + 4 + 24 * valid.manifest.memberIds.size();
    auto rehash = [](std::vector<std::uint8_t>& raw) {
        const auto h = fnv1a64(raw.data(), raw.size() - 8);
        for (unsigned i = 0; i < 8; ++i) raw[raw.size() - 8 + i] = std::uint8_t(h >> (8 * i));
    };
    auto write32 = [](std::vector<std::uint8_t>& raw, std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) raw[at + i] = std::uint8_t(value >> (8 * i));
    };
    auto rejectWire = [&](std::vector<std::uint8_t> raw) {
        rehash(raw);
        auto unchanged = valid;
        std::string error;
        CHECK(!parseBundle(raw.data(), raw.size(), unchanged, error));
        CHECK(unchanged.manifest == valid.manifest && unchanged.railVolts == valid.railVolts &&
              unchanged.railDemandA == valid.railDemandA && unchanged.bleed == valid.bleed);
    };
    // A correct non-cryptographic integrity hash is not semantic validation. Each payload
    // region must reject NaN/Infinity before any sample can reach a replayer.
    for (std::size_t at : {payloadAt, payloadAt + 4, payloadAt + 8}) {
        for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
            auto raw = bytes;
            write32(raw, at, std::bit_cast<std::uint32_t>(bad));
            rejectWire(std::move(raw));
        }
    }
    // Duplicate/reordered member IDs, unsupported versions, and wrapped sample ranges are rejected.
    auto raw = bytes;
    std::copy_n(raw.begin() + std::ptrdiff_t(countAt + 4), 16, raw.begin() + std::ptrdiff_t(countAt + 4 + 24));
    rejectWire(std::move(raw));
    raw = bytes; write32(raw, 4, kBundleVersion + 1); rejectWire(std::move(raw));
    raw = bytes; for (unsigned i = 0; i < 8; ++i) raw[16 + i] = 0xFF; rejectWire(std::move(raw));

    auto rejectMemory = [&](const InfluenceBundle& bad) {
        BundleReplayer rp; std::string error;
        CHECK(serializeBundle(bad).empty());
        CHECK(rp.open(valid, valid.manifest, error));
        CHECK(!rp.open(bad, bad.manifest, error));
        float left{}, right{};
        CHECK(!rp.readBleed(0, 0, 1, &left, &right));  // failed open discards previous authorization
    };
    auto bad = valid; bad.manifest.lengthSamples = std::numeric_limits<std::uint64_t>::max();
    bad.railVolts.clear(); bad.railDemandA.clear(); bad.bleed.clear(); rejectMemory(bad);
    bad = valid; bad.manifest.memberIds.clear(); bad.manifest.inputHashes.clear(); bad.bleed.clear(); rejectMemory(bad);
    bad = valid; bad.manifest.inputHashes.clear(); rejectMemory(bad);
    bad = valid; bad.railVolts[0] = std::numeric_limits<float>::infinity(); rejectMemory(bad);
    bad = valid; bad.bleed.pop_back(); rejectMemory(bad);
    bad = valid; std::swap(bad.manifest.memberIds[0], bad.manifest.memberIds[1]); rejectMemory(bad);
    bad = valid; bad.manifest.bleedGainLinear = -1; rejectMemory(bad);
    bad = valid; bad.manifest.sourceOhms = -1; rejectMemory(bad);

    // Offline bundles have no live ring-size limit on their requested bleed delay.
    auto delayed = valid; delayed.manifest.latencyQuanta = 49;
    BundleReplayer rp; std::string error;
    CHECK(rp.open(delayed, delayed.manifest, error));
    const auto delayedBytes = serializeBundle(delayed);
    InfluenceBundle parsed;
    CHECK(parseBundle(delayedBytes.data(), delayedBytes.size(), parsed, error));
    CHECK(parsed.manifest.latencyQuanta == 49);

    // Co-render uses the same validated output contract and leaves prior results untouched.
    std::vector<MemberStem> stems(2);
    for (std::size_t i = 0; i < stems.size(); ++i) {
        stems[i].id = mid(i + 1);
        stems[i].left.assign(kQuantumFrames, 0.1f);
        stems[i].right.assign(kQuantumFrames, 0.1f);
    }
    CoRenderConfig cfg; cfg.lengthSamples = kQuantumFrames;
    const DemandFn ordinary = [](std::size_t, std::uint64_t, double) { return 0.01; };
    auto rejectRender = [&](const CoRenderConfig& config, const std::vector<MemberStem>& input, const DemandFn& demand) {
        auto unchanged = valid;
        CHECK(!coRender(config, input, demand, unchanged, error));
        CHECK(unchanged.manifest == valid.manifest && unchanged.railVolts == valid.railVolts &&
              unchanged.railDemandA == valid.railDemandA && unchanged.bleed == valid.bleed);
    };
    auto invalid = cfg; invalid.startSample = std::numeric_limits<std::uint64_t>::max();
    rejectRender(invalid, stems, ordinary);
    invalid = cfg; invalid.tolerance = std::numeric_limits<double>::infinity(); rejectRender(invalid, stems, ordinary);
    invalid = cfg; invalid.modelVersion.assign(257, 'x'); rejectRender(invalid, stems, ordinary);
    invalid = cfg; invalid.sourceOhms = 0; invalid.nominalRailV = 1e100; rejectRender(invalid, stems, ordinary);
    invalid = cfg; invalid.sourceOhms = 0;
    rejectRender(invalid, stems, [](std::size_t, std::uint64_t, double) { return 1e100; });
    auto invalidStems = stems; invalidStems[0].left[0] = std::numeric_limits<float>::quiet_NaN();
    rejectRender(cfg, invalidStems, ordinary);
    invalid = cfg; invalid.topology = {{0.f, std::numeric_limits<float>::infinity()}, {0.f, 0.f}};
    rejectRender(invalid, stems, ordinary);
    cfg.latencyQuanta = 49;
    CHECK(coRender(cfg, stems, ordinary, parsed, error));
    CHECK(!serializeBundle(parsed).empty());
}
}  // namespace

int main() {
    testNoexceptContract();
    testJoinRules();
    testIsolation();
    testAggregationOrderIndependence(2);
    testAggregationOrderIndependence(32);
    testRowSumBound();
    testMissingLateAndDecay();
    testDeathAndBroker();
    testHostilePeer();
    testConcurrentNoWaits();
    testValidateLayout();
    testSharedMemory();
    testBundle();
    testBundleValidation();
    if (failures) { std::printf("%d network check(s) FAILED\n", failures); return 1; }
    std::printf("network tests passed\n");
    return 0;
}
