// CircuitDrift tests: T11/P16 deterministic co-render and influence-bundle replay —
// N cd::runtime::Engine instances co-rendered in fixed member-ID order with one PSU fixed-point
// solve per quantum, then replayed from the recorded bundle in arbitrary callback orders and block
// partitions {1,17,64,127,512,8192}: peak error vs reference <= 1e-6 (measured, expected 0).
// Also: member-order invariance of the bundle, bundle serialization round trip and hostile-byte
// rejection, replay invalidation on content/automation/engine/Fs/routing mismatch and start
// range, unconverged rail solve rejection, RtTrap on the replay audio path.
// Circuits, stems and the PSU/demand model are SYNTHETIC fixtures; nothing here validates hardware.
// Owner: network agent.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "network/CoRender.h"
#include "runtime/Engine.h"
#include "support/RtTrap.h"

namespace {
using namespace cd::network;
using cd::test::RtTrapCounts;
using cd::test::RtTrapScope;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

constexpr double kFs = 48000;

struct Lcg {
    std::uint64_t s;
    std::uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return std::uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
};

std::vector<CoRenderMember> makeMembers(std::uint32_t n, std::int64_t length, std::uint64_t seed, bool mixedLayout,
                                        cd::runtime::QualityMode q = cd::runtime::QualityMode::Studio) {
    std::vector<CoRenderMember> ms(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto& m = ms[i];
        m.id = Uuid::fromSeed(seed + i);
        const int ch = (mixedLayout && (i % 2)) ? 1 : 2;
        m.engine.sampleRate = kFs;
        m.engine.channels = ch;
        m.engine.maxBlock = 512;
        m.engine.quality.mode = q;
        m.powerW = 10.0 + 3.0 * i;
        Lcg g{seed * 31 + i};
        m.stem.assign(std::size_t(ch), std::vector<double>(std::size_t(length)));
        const double f = 110.0 * (1 + i % 5) + 7.0 * i;
        for (std::int64_t s = 0; s < length; ++s) {
            const double env = (s / 2400) % 2 ? 0.9 : 0.2;   // bursts: activity (demand) changes
            for (int c = 0; c < ch; ++c)
                m.stem[std::size_t(c)][std::size_t(s)] = env * (0.8 * std::sin(2 * M_PI * f * double(s) / kFs + c) + 0.2 * g.d());
        }
    }
    return ms;
}

CoRenderConfig baseConfig(std::int64_t length, std::int64_t start = 4800) {
    CoRenderConfig c;
    c.sampleRate = kFs;
    c.start = start;
    c.length = length;
    c.bleed = BleedConfig{true, -24.0};
    c.bleedSeed = 9;
    return c;
}

const CoRenderMember& memberById(const std::vector<CoRenderMember>& ms, const Uuid& id) {
    for (const auto& m : ms) if (m.id == id) return m;
    return ms.front();
}

RtTrapCounts g_trap{};

// Replays every member from the bundle; callbacks interleave members in a seeded random order and
// each member uses `partition` (0 = random sizes from the canonical set).
std::vector<Stem> replay(const CoRenderResult& res, const std::vector<CoRenderMember>& ms, std::uint32_t partition,
                         std::uint64_t orderSeed, std::string& error) {
    const auto& b = res.bundle;
    const std::size_t n = b.members.size();
    std::vector<std::unique_ptr<cd::runtime::Engine>> eng(n);
    std::vector<ReplaySource> src(n);
    std::vector<Stem> out(n);
    std::vector<std::int64_t> pos(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const CoRenderMember& m = memberById(ms, b.members[i].id);
        eng[i] = std::make_unique<cd::runtime::Engine>();
        if (!eng[i]->prepare(m.engine, error)) return {};
        if (!src[i].bind(b, m.id, stemContentHash(m.stem), automationHash(), engineIdentityHash(*eng[i]), kFs, error)) return {};
        out[i].assign(m.stem.size(), std::vector<double>(std::size_t(b.length), 0.0));
    }
    static const std::uint32_t parts[] = {1, 17, 64, 127, 512, 8192};
    Lcg r{orderSeed};
    std::vector<double> inBuf(2 * 8192), bleedBuf(2 * 8192);
    for (;;) {
        std::vector<std::size_t> live;
        for (std::size_t i = 0; i < n; ++i) if (pos[i] < b.length) live.push_back(i);
        if (live.empty()) break;
        const std::size_t i = live[r.u() % live.size()];
        const CoRenderMember& m = memberById(ms, b.members[i].id);
        const std::uint32_t p = partition ? partition : parts[r.u() % 6];
        const std::uint32_t frames = std::uint32_t(std::min<std::int64_t>(p, b.length - pos[i]));
        const std::size_t ch = m.stem.size();
        double* bl[2] = {bleedBuf.data(), bleedBuf.data() + 8192};
        const double* ip[2] = {inBuf.data(), inBuf.data() + 8192};
        double* op[2] = {out[i][0].data() + pos[i], ch > 1 ? out[i][1].data() + pos[i] : nullptr};
        cd::runtime::ProcessBlock blk;
        blk.ctx.sampleRate = kFs;
        blk.ctx.sampleStart = b.start + pos[i];
        blk.ctx.frames = frames;
        blk.buses = cd::runtime::AudioBuses{int(ch), ip, op};
        {
            RtTrapScope trap;
            src[i].bleedFor(b.start + pos[i], frames, bl);
            for (std::size_t c = 0; c < ch; ++c)
                for (std::uint32_t f = 0; f < frames; ++f)
                    inBuf[c * 8192 + f] = m.stem[c][std::size_t(pos[i]) + f] + bl[c][f];
            eng[i]->process(blk);
            const auto t = trap.counts();
            g_trap.allocations += t.allocations; g_trap.deallocations += t.deallocations; g_trap.mutexLocks += t.mutexLocks;
            g_trap.condWaits += t.condWaits; g_trap.ioCalls += t.ioCalls;
        }
        pos[i] += frames;
    }
    return out;
}

double peakDiff(const std::vector<Stem>& a, const std::vector<Stem>& b) {
    double d = 0;
    if (a.size() != b.size()) return INFINITY;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t c = 0; c < a[i].size(); ++c)
            for (std::size_t s = 0; s < a[i][c].size(); ++s) {
                const double e = std::fabs(a[i][c][s] - b[i][c][s]);
                if (!(e <= d)) d = std::isfinite(e) ? std::max(d, e) : INFINITY;
            }
    return d;
}

double peakAbs(const std::vector<Stem>& a) {
    double d = 0;
    for (const auto& m : a) for (const auto& c : m) for (double x : c) d = std::max(d, std::fabs(x));
    return d;
}

// ---------------------------------------------------------------------------------------------
void replayEquivalence(std::uint32_t n, std::int64_t length, bool mixed, cd::runtime::QualityMode q, const char* label,
                       bool allPartitions) {
    auto ms = makeMembers(n, length, 100 + n, mixed, q);
    // keep the group's total demand inside what the synthetic reservoir can supply
    if (n > 8) for (std::uint32_t i = 0; i < n; ++i) ms[i].powerW = 1.0 + 0.1 * i;
    CoRenderResult res;
    std::string err;
    CHECK(coRender(ms, baseConfig(length), res, err), "%s coRender: %s", label, err.c_str());
    if (!res.bundle.integrityHash) return;
    // the bleed must actually change the output (otherwise equivalence is vacuous)
    CoRenderConfig dry = baseConfig(length);
    dry.bleed.enabled = false;
    CoRenderResult resDry;
    CHECK(coRender(ms, dry, resDry, err), "dry coRender: %s", err.c_str());
    const double influence = peakDiff(res.reference, resDry.reference);
    double bleedPeak = 0;
    for (const auto& m : res.bundle.members) for (const auto& c : m.bleed) for (double x : c) bleedPeak = std::max(bleedPeak, std::fabs(x));
    CHECK(influence > 1e-4 && bleedPeak > 1e-3, "%s: bleed has no audible influence (%.3e / %.3e)", label, influence, bleedPeak);
    CHECK(res.maxIterationsUsed >= 1 && res.maxIterationsUsed <= 8, "iterations %d", res.maxIterationsUsed);
    double worst = 0;
    int runs = 0;
    const auto t0 = g_trap;
    std::vector<std::uint32_t> partitions = {0, 0, 0};
    if (allPartitions) partitions = {1, 17, 64, 127, 512, 8192, 0, 0};
    for (std::uint32_t p : partitions) {
        const std::uint64_t seed = 7 + 13 * runs;
        const auto out = replay(res, ms, p, seed, err);
        CHECK(!out.empty(), "%s replay p=%u: %s", label, p, err.c_str());
        if (out.empty()) continue;
        const double d = peakDiff(out, res.reference);
        worst = std::max(worst, d);
        CHECK(d <= 1e-6, "%s: replay p=%u order %llu peak error %.3e", label, p, (unsigned long long)seed, d);
        CHECK(std::isfinite(peakAbs(out)), "non-finite replay");
        ++runs;
    }
    const long viol = g_trap.violations() - t0.violations();
    CHECK(viol == 0, "%s: %ld RT violations on the replay path", label, viol);
    double rmin = 1e300, rmax = 0;
    for (double v : res.bundle.rail) { rmin = std::min(rmin, v); rmax = std::max(rmax, v); }
    std::printf("T11/P16 replay %s: %u members x %lld samples, %d replays (callback orders x partitions%s): peak error %.3e "
                "(<= 1e-6; %s), bleed influence on output %.3e, rail %.4f..%.4f V (synthetic), fixed-point <= %d iterations, "
                "RT violations %ld\n",
                label, n, (long long)length, runs, allPartitions ? " {1,17,64,127,512,8192,random}" : " random",
                worst, worst == 0.0 ? "bit-identical" : "not bit-identical", influence, rmin, rmax, res.maxIterationsUsed, viol);
}

void bundleDeterminismAndSerialization() {
    const std::int64_t len = 3000;
    auto ms = makeMembers(3, len, 300, true);
    CoRenderResult a, b;
    std::string err;
    CHECK(coRender(ms, baseConfig(len), a, err), "coRender: %s", err.c_str());
    std::reverse(ms.begin(), ms.end());   // input order must not matter (fixed member-ID order)
    CHECK(coRender(ms, baseConfig(len), b, err), "coRender reversed: %s", err.c_str());
    CHECK(a.bundle.integrityHash == b.bundle.integrityHash && peakDiff(a.reference, b.reference) == 0.0, "member order changed the bundle");
    for (std::size_t k = 1; k < a.bundle.members.size(); ++k)
        CHECK(uuidLess(a.bundle.members[k - 1].id, a.bundle.members[k].id), "bundle not in member-ID order");
    const auto bytes = a.bundle.serialize();
    InfluenceBundle p;
    CHECK(InfluenceBundle::parse(bytes, p, err) && p.integrityHash == a.bundle.integrityHash && p.computeHash() == a.bundle.integrityHash,
          "round trip: %s", err.c_str());
    CHECK(p.members.size() == 3 && p.members[1].bleed == a.bundle.members[1].bleed && p.rail == a.bundle.rail, "round trip content");
    // hostile bytes: every single-byte change and every truncation is rejected
    Lcg r{77};
    int rejected = 0;
    for (int it = 0; it < 1000; ++it) {
        auto m = bytes;
        const std::size_t off = r.u() % m.size();
        m[off] = std::uint8_t(m[off] ^ (1u + r.u() % 255));
        InfluenceBundle q;
        if (!InfluenceBundle::parse(m, q, err)) ++rejected;
    }
    int truncRejected = 0;
    for (int it = 0; it < 200; ++it) {
        std::vector<std::uint8_t> m(bytes.begin(), bytes.begin() + std::ptrdiff_t(r.u() % bytes.size()));
        InfluenceBundle q;
        if (!InfluenceBundle::parse(m, q, err)) ++truncRejected;
    }
    auto ext = bytes;
    ext.push_back(0);
    InfluenceBundle q;
    CHECK(!InfluenceBundle::parse(ext, q, err), "trailing byte accepted");
    CHECK(rejected == 1000 && truncRejected == 200, "corrupted bundles accepted (%d/1000, %d/200)", rejected, truncRejected);
    std::printf("T11/P16 bundle: member-order invariant (hash %016llx), %zu-byte v%u serialization round trip, 1000/1000 byte "
                "mutations + 200/200 truncations + trailing data rejected\n",
                (unsigned long long)a.bundle.integrityHash, bytes.size(), InfluenceBundle::kVersion);
}

void replayInvalidation() {
    const std::int64_t len = 2048;
    const auto ms = makeMembers(2, len, 400, false);
    CoRenderResult res;
    std::string err;
    CHECK(coRender(ms, baseConfig(len, 1000), res, err), "coRender: %s", err.c_str());
    cd::runtime::Engine e;
    CHECK(e.prepare(ms[0].engine, err), "prepare");
    const auto id = engineIdentityHash(e);
    const auto ch = stemContentHash(ms[0].stem);
    ReplaySource s;
    CHECK(s.bind(res.bundle, ms[0].id, ch, automationHash(), id, kFs, err), "valid bind: %s", err.c_str());
    Stem edited = ms[0].stem;
    edited[0][100] += 1e-9;   // any content change invalidates
    CHECK(!s.bind(res.bundle, ms[0].id, stemContentHash(edited), automationHash(), id, kFs, err) && err.find("content") != std::string::npos, "edited stem accepted");
    CHECK(!s.bind(res.bundle, ms[0].id, ch, automationHash() ^ 1, id, kFs, err) && err.find("automation") != std::string::npos, "automation mismatch accepted");
    CHECK(!s.bind(res.bundle, Uuid::fromSeed(999), ch, automationHash(), id, kFs, err) && err.find("routing") != std::string::npos, "foreign member accepted");
    CHECK(!s.bind(res.bundle, ms[0].id, ch, automationHash(), id, 44100, err) && err.find("sample rate") != std::string::npos, "Fs mismatch accepted");
    auto cfgDraft = ms[0].engine;
    cfgDraft.quality.mode = cd::runtime::QualityMode::Eco;
    cd::runtime::Engine e2;
    CHECK(e2.prepare(cfgDraft, err), "prepare eco");
    const bool idDiffers = engineIdentityHash(e2) != id;
    CHECK(idDiffers && !s.bind(res.bundle, ms[0].id, ch, automationHash(), engineIdentityHash(e2), kFs, err), "engine identity mismatch accepted");
    InfluenceBundle tampered = res.bundle;
    tampered.members[1].bleed[0][5] += 1e-12;
    CHECK(!s.bind(tampered, ms[1].id, stemContentHash(ms[1].stem), automationHash(), id, kFs, err) && err.find("integrity") != std::string::npos,
          "tampered stream accepted");
    // start/range: outside [start, start+length) -> zero bleed + flag (replay invalid there)
    CHECK(s.bind(res.bundle, ms[0].id, ch, automationHash(), id, kFs, err), "rebind");
    double buf[2][64];
    double* o[2] = {buf[0], buf[1]};
    const std::uint32_t f1 = s.bleedFor(1000 - 10, 64, o);
    const InfluenceBundle::Member* bm = nullptr;
    for (const auto& m : res.bundle.members) if (m.id == ms[0].id) bm = &m;
    CHECK(bm && (f1 & ReplaySource::kOutOfRange) && buf[0][0] == 0.0 && buf[0][10] == bm->bleed[0][0], "start edge");
    const std::uint32_t f2 = s.bleedFor(1000 + len - 32, 64, o);
    CHECK((f2 & ReplaySource::kOutOfRange) && buf[1][40] == 0.0, "end edge");
    CHECK(s.bleedFor(1000 + 64, 64, o) == 0 && s.railAt(1000 + 200) == res.bundle.rail[200 / 128], "in range");
    RtTrapCounts t;
    {
        RtTrapScope trap;
        for (int k = 0; k < 100; ++k) s.bleedFor(1000 + 64 * k, 64, o);
        t = trap.counts();
    }
    CHECK(t.violations() == 0, "bleedFor RT violations %ld", t.violations());
    std::printf("T11/P16 replay invalidation: content/automation/routing/Fs/engine-identity/integrity mismatches refused; "
                "out-of-range samples get zero bleed + flag; bleedFor RT-clean\n");
}

void unconvergedRejected() {
    const std::int64_t len = 1024;
    const auto ms = makeMembers(2, len, 500, false);
    CoRenderConfig c = baseConfig(len);
    c.psu.sourceOhm = 1000;
    c.psu.capacitanceF = 1e-9;   // stiff, heavily loaded rail: slow fixed-point contraction
    std::vector<CoRenderMember> heavy = ms;
    for (auto& m : heavy) m.powerW = 30;
    CoRenderResult res;
    std::string err;
    const bool ok = coRender(heavy, c, res, err);
    CHECK(!ok && err.find("unconverged") != std::string::npos, "unconverged solve not rejected (%s)", err.c_str());
    const std::string why = err;
    CoRenderConfig bad = baseConfig(len);
    bad.maxIterations = 9;
    CHECK(!coRender(ms, bad, res, err), "9 iterations accepted");
    std::printf("T11/P16 nonlinear rail: unconverged 8-iteration fixed point rejects the render (%s); iteration bound > 8 refused\n",
                why.c_str());
}

}  // namespace

int main() {
    replayEquivalence(2, 12000, false, cd::runtime::QualityMode::Studio, "2 stereo", true);
    replayEquivalence(4, 6000, true, cd::runtime::QualityMode::Studio, "4 mixed mono/stereo", true);
    replayEquivalence(32, 1536, true, cd::runtime::QualityMode::Eco, "32 mixed (Eco)", false);
    bundleDeterminismAndSerialization();
    replayInvalidation();
    unconvergedRejected();
    if (g_fail) { std::printf("network_corender: %d failure(s)\n", g_fail); return 1; }
    std::printf("network_corender: all checks passed\n");
    return 0;
}
