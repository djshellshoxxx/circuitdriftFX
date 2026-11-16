// T11 (CD-F11) protocol, prepared ring, bleed law and SYNTHETIC reservoir tests. In-process mocks
// only (HeapRingAllocator); real OS shared memory / broker process / host matrix: G05 open.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>
#include "network/Bleed.h"
#include "network/PreparedRing.h"
#include "network/Protocol.h"
#include "network/SharedPsu.h"
#include "support/RtTrap.h"

namespace {
using namespace cd::network;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static_assert(std::is_trivially_copyable_v<FrameHeader> && sizeof(FrameHeader) == 112);
static_assert(std::is_trivially_copyable_v<DemandFrame> && std::is_trivially_copyable_v<ResultFrame>);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(PreparedRing<DemandFrame>::kSlots == 64 && kQuantumFrames == 128 && kLiveLatencyQuanta == 8);

const Uuid128 kG{1, 2}, kS{3, 4}, kM{5, 6};

FrameHeader goodHeader() {
    FrameHeader h;
    stampHeader<DemandPayload>(h);
    h.group = kG; h.session = kS; h.member = kM; h.generation = 7; h.sampleRate = 48000; h.channels = 2;
    h.epoch = 3; h.sampleStart = 1280; h.sequence = 10;
    return h;
}
FrameExpectation expectation() {
    FrameExpectation x;
    x.type = PayloadType::Demand; x.group = kG; x.session = kS; x.member = kM; x.sampleRate = 48000; x.epoch = 3;
    x.minGeneration = 5;
    return x;
}

void headerValidation() {
    const auto x = expectation();
    CHECK(validate(goodHeader(), x) == FrameError::None);
    { auto h = goodHeader(); h.magic[3] = 'Q'; CHECK(validate(h, x) == FrameError::BadMagic); }
    { auto h = goodHeader(); h.major = kProtocolMajor + 1; CHECK(validate(h, x) == FrameError::IncompatibleMajor); }
    { auto h = goodHeader(); h.minor = 9; CHECK(validate(h, x) == FrameError::None); }   // minor is compatible
    { auto h = goodHeader(); h.byteSize += 8; CHECK(validate(h, x) == FrameError::InvalidSize); }
    { auto h = goodHeader(); h.payloadLength = 0; CHECK(validate(h, x) == FrameError::InvalidSize); }
    { FrameHeader h = goodHeader(); auto y = x; y.type = PayloadType::Result; CHECK(validate(h, y) == FrameError::WrongPayloadType); }
    { auto h = goodHeader(); h.group.lo ^= 1; CHECK(validate(h, x) == FrameError::WrongGroup); }
    { auto h = goodHeader(); h.session.hi ^= 1; CHECK(validate(h, x) == FrameError::WrongSession); }
    { auto h = goodHeader(); h.member.lo ^= 1; CHECK(validate(h, x) == FrameError::WrongMember); }
    { auto h = goodHeader(); h.member.lo ^= 1; auto y = x; y.member = {}; CHECK(validate(h, y) == FrameError::None); }
    { auto h = goodHeader(); h.sampleRate = 44100; CHECK(validate(h, x) == FrameError::SampleRateMismatch); }
    { auto h = goodHeader(); h.epoch = 2; CHECK(validate(h, x) == FrameError::EpochMismatch); }
    { auto h = goodHeader(); h.generation = 4; CHECK(validate(h, x) == FrameError::StaleGeneration); }
    { auto h = goodHeader(); h.channels = 3; CHECK(validate(h, x) == FrameError::InvalidChannels); }
    { auto h = goodHeader(); h.channels = 0; CHECK(validate(h, x) == FrameError::InvalidChannels); }
    for (std::uint32_t e = 0; e < static_cast<std::uint32_t>(FrameError::Count); ++e)
        CHECK(std::strlen(toString(static_cast<FrameError>(e))) > 0);
    std::mt19937_64 rng(1);
    const Uuid128 a = makeUuidV4(rng), b = makeUuidV4(rng);
    CHECK(!(a == b) && !a.isNil() && ((a.hi >> 12) & 0xF) == 4 && (a.lo >> 62) == 2);
    CHECK(std::fabs(liveDisplayDelaySeconds(48000) - 1024.0 / 48000.0) < 1e-15);
    CHECK(std::strcmp(kLiveCouplingLabel, "approximate live coupling") == 0);
}

void ringSemantics() {
    HeapRingAllocator alloc;
    RegionRequest r;
    r.bytes = PreparedRing<DemandFrame>::regionBytes();
    PreparedRing<DemandFrame> ring(alloc.allocate(r));
    CHECK(ring.control().slotCount == 64 && ring.control().slotBytes == sizeof(DemandFrame));
    long violations = 0;
    std::uint64_t written = 0, read = 0;
    {
        cd::test::RtTrapScope trap;
        for (int round = 0; round < 10; ++round) {
            int pushed = 0;
            while (DemandFrame* f = ring.beginWrite()) {
                f->header.sequence = written++;
                f->payload.tap[0][0] = double(f->header.sequence);
                ring.commit();
                ++pushed;
            }
            CHECK(pushed == (round == 0 ? 64 : 37));
            const std::size_t n = ring.acquire();
            CHECK(n == 64);
            for (std::size_t i = 0; i < 37; ++i) {
                CHECK(ring.at(i).header.sequence == read && ring.at(i).payload.tap[0][0] == double(read));
                ++read;
            }
            ring.release(37);
        }
        violations = trap.counts().violations();
    }
    CHECK(violations == 0);
    CHECK(ring.producerDrops() == 10);
    CHECK(ring.sizeApprox() == 27);
    alloc.failNext(true);
    ChannelIdentity id;
    bool threw = false;
    try { MemberChannel ch(alloc, id); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    RegionRequest small;
    small.bytes = 64;
    threw = false;
    try { PreparedRing<DemandFrame> bad(alloc.allocate(small)); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

// Two threads: every published frame must be read complete (release/acquire publication).
void ringThreaded() {
    HeapRingAllocator alloc;
    RegionRequest r;
    r.bytes = PreparedRing<ResultFrame>::regionBytes();
    PreparedRing<ResultFrame> ring(alloc.allocate(r));
    constexpr std::uint64_t kFrames = 20000;
    std::atomic<bool> go{true};
    std::thread producer([&] {
        std::uint64_t seq = 0;
        while (seq < kFrames && go.load()) {
            ResultFrame* f = ring.beginWrite();
            if (!f) { std::this_thread::yield(); continue; }
            f->header.sequence = seq;
            for (auto& ch : f->payload.bleed) for (double& s : ch) s = double(seq);
            ring.commit();
            ++seq;
        }
    });
    std::uint64_t expect = 0;
    bool torn = false, order = true;
    while (expect < kFrames) {
        const std::size_t n = ring.acquire();
        if (n == 0) { std::this_thread::yield(); continue; }
        for (std::size_t i = 0; i < n; ++i) {
            const ResultFrame& f = ring.at(i);
            if (f.header.sequence != expect) order = false;
            for (const auto& ch : f.payload.bleed) for (double s : ch) if (s != double(expect)) torn = true;
            ++expect;
        }
        ring.release(n);
    }
    go = false;
    producer.join();
    CHECK(order && !torn);
}

void bleedValidation() {
    BleedConfig c;
    CHECK(validateBleed(c) == BleedError::None && std::fabs(bleedGain(c) - std::pow(10.0, -4.5)) < 1e-18);
    c.levelDb = -24; CHECK(validateBleed(c) == BleedError::None);
    c.levelDb = -23.9; CHECK(validateBleed(c) == BleedError::LevelOutOfRange);
    c.levelDb = -91; CHECK(validateBleed(c) == BleedError::LevelOutOfRange);
    c.levelDb = NAN; CHECK(validateBleed(c) == BleedError::LevelOutOfRange);
    c.off = true; CHECK(validateBleed(c) == BleedError::None && bleedGain(c) == 0.0);
    BleedConfig t;
    t.levelDb = -30;
    const double g = bleedGain(t);
    const Uuid128 a{0, 1}, b{0, 2}, d{0, 3};
    t.topology = BleedTopology{{a, b, d}, {0, g * 0.5, g * 0.5, g, 0, 0, -g * 0.25, g * 0.75, 0}};
    CHECK(validateBleed(t) == BleedError::None);
    auto bad = t;
    bad.topology->weights[1] = g * 0.6;            // row 0: 1.1 g
    CHECK(validateBleed(bad) == BleedError::TopologyRowSumExceedsGain);
    bad = t; bad.topology->weights[0] = 1e-9;      CHECK(validateBleed(bad) == BleedError::TopologyDiagonal);
    bad = t; bad.topology->weights.pop_back();     CHECK(validateBleed(bad) == BleedError::TopologyNotSquare);
    bad = t; bad.topology->members[2] = a;         CHECK(validateBleed(bad) == BleedError::TopologyDuplicateMember);
    bad = t; bad.topology->weights[3] = INFINITY;  CHECK(validateBleed(bad) == BleedError::TopologyNonFinite);
    // Default weights: exact row sum g for N = 2 and 32; signs depend on (seed, UUID), not order.
    for (std::size_t n : {2u, 32u}) {
        std::vector<Uuid128> ids;
        for (std::size_t i = 0; i < n; ++i) ids.push_back({0x1000 + i, i * 7919});
        std::vector<double> W;
        CHECK(buildBleedWeights(BleedConfig{false, -30, 99, {}}, ids, W) == BleedError::None);
        CHECK(std::fabs(maxAbsRowSum(W, n) - g) <= 1e-15);
        for (std::size_t i = 0; i < n; ++i) CHECK(W[i * n + i] == 0.0);
        std::vector<Uuid128> rev(ids.rbegin(), ids.rend());
        std::vector<double> W2;
        (void)buildBleedWeights(BleedConfig{false, -30, 99, {}}, rev, W2);
        CHECK(W2[0 * n + 1] == W[(n - 1) * n + (n - 2)]);   // same source member -> same signed weight
    }
    int plus = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) plus += bleedSign(5, {i, ~i}) > 0;
    CHECK(plus > 430 && plus < 570);
}

// Channel routing: mono -> stereo at -3 dB each, stereo -> mono = mean, same-channel stereo.
void bleedRouting() {
    const double g = std::pow(10.0, -24 / 20.0);
    std::vector<double> W = {0, g, g, 0};
    double xm[4] = {1, -1, 0.5, 0}, xl[4] = {1, 0, 0, 0}, xr[4] = {0, 1, 0, 0};
    double oL[4], oR[4], om[4];
    BleedSource src[2];
    src[0].ch[0] = xm; src[0].channels = 1;
    src[1].ch[0] = xl; src[1].ch[1] = xr; src[1].channels = 2;
    BleedSink snk[2];
    snk[0].ch[0] = om; snk[0].channels = 1;
    snk[1].ch[0] = oL; snk[1].ch[1] = oR; snk[1].channels = 2;
    long v;
    { cd::test::RtTrapScope trap; mixBleed(W, 2, src, snk, 4); v = trap.counts().violations(); }
    CHECK(v == 0);
    CHECK(std::fabs(oL[0] - g * kMonoToStereoGain) < 1e-18 && std::fabs(oR[1] + g * kMonoToStereoGain) < 1e-18);
    CHECK(std::fabs(om[0] - 0.5 * g) < 1e-18 && std::fabs(om[1] - 0.5 * g) < 1e-18 && om[2] == 0.0);
    snk[0].enabled = false;
    mixBleed(W, 2, src, snk, 4);
    CHECK(om[0] == 0.0 && om[1] == 0.0);
}

// SYNTHETIC reservoir: closed-form conducting steady state and diode-off discharge.
void syntheticPsu() {
    PsuSpec s;
    ReservoirPsu p(s, 48000);
    CHECK(p.railFraction() == 1.0);
    for (int i = 0; i < 4000; ++i) (void)p.stepQuantum(0.4);
    CHECK(std::fabs(p.volts() - (s.sourceV - 0.4 * s.sourceResistanceOhm)) < 1e-9);
    p.setVolts(s.sourceV + 5);
    const double dt = 128.0 / 48000;
    CHECK(std::fabs(p.trial(p.volts(), 0.1) - (s.sourceV + 5 - 0.1 * dt / s.capacitanceF)) < 1e-9);
    CHECK(p.trial(10, NAN) >= 0 && std::isfinite(p.trial(NAN, 1)));
    PsuSpec bad = s; bad.capacitanceF = 0; CHECK(!validPsuSpec(bad));
    CHECK(validPsuSpec(s));
}

}  // namespace

int main() {
    headerValidation();
    ringSemantics();
    ringThreaded();
    bleedValidation();
    bleedRouting();
    syntheticPsu();
    std::printf("network protocol T11: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
