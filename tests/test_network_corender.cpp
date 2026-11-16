// T11/P16 deterministic co-render, influence bundle replay and measured cross-talk bounds.
// SYNTHETIC demand/PSU models and white-noise fixtures; no host qualification (G05 open).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include "network/Bleed.h"
#include "network/CoRender.h"
#include "support/RtTrap.h"

namespace {
using namespace cd::network;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

CoRenderSpec makeSpec(std::size_t members, std::uint64_t length, std::uint64_t seed) {
    CoRenderSpec s;
    s.sampleRate = 48000;
    s.startSample = 96000 + 37;
    s.lengthSamples = length;
    s.bleed.levelDb = -30;
    s.bleed.seed = 4242;
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(-1, 1);
    for (std::size_t i = 0; i < members; ++i) {
        CoRenderMemberInput m;
        m.member = {rng(), rng()};
        m.channels = i % 4 == 1 ? 1 : 2;
        m.audio.assign(m.channels, std::vector<double>(length));
        for (auto& ch : m.audio) for (double& v : ch) v = u(rng) * (0.2 + 0.8 * double(i % 3) / 2.0);
        const std::size_t quanta = (length + 127) / 128;
        for (std::size_t q = 0; q < quanta; ++q) m.activity.push_back(0.5 + 0.5 * std::sin(0.01 * double(q) + double(i)));
        m.quiescentA = 0.02 + 0.002 * double(i);
        m.signalA = 0.05;
        s.members.push_back(std::move(m));
    }
    return s;
}

// A host-like delivery plan: random member order and random block partition.
std::vector<FeedStep> randomPlan(const CoRenderSpec& s, std::uint64_t seed, std::size_t maxBlock) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint64_t> left(s.members.size(), s.lengthSamples);
    std::vector<FeedStep> plan;
    for (;;) {
        std::vector<std::size_t> live;
        for (std::size_t i = 0; i < left.size(); ++i) if (left[i]) live.push_back(i);
        if (live.empty()) break;
        const std::size_t i = live[rng() % live.size()];
        const std::size_t n = std::min<std::uint64_t>(left[i], 1 + rng() % maxBlock);
        plan.push_back({i, n});
        left[i] -= n;
    }
    return plan;
}

double peakDiff(const InfluenceBundle& a, const InfluenceBundle& b) {
    double p = 0;
    for (std::size_t q = 0; q < a.railFraction.size(); ++q) p = std::max(p, std::fabs(a.railFraction[q] - b.railFraction[q]));
    for (std::size_t i = 0; i < a.bleed.size(); ++i)
        for (std::size_t c = 0; c < a.bleed[i].size(); ++c)
            for (std::size_t s = 0; s < a.bleed[i][c].size(); ++s) p = std::max(p, std::fabs(a.bleed[i][c][s] - b.bleed[i][c][s]));
    return p;
}

void replayIndependentOfHostOrder() {
    for (std::size_t members : {2u, 32u}) {
        const auto spec = makeSpec(members, members == 2 ? 48000 + 77 : 12000 + 5, 17 + members);
        const auto ref = coRender(spec);   // sequential reference
        CHECK(ref.ok());
        if (!ref.ok()) std::printf("  %s\n", ref.reason.c_str());
        CHECK(ref.maxIterationsUsed >= 1 && ref.maxIterationsUsed <= kMaxFixedPointIterations);
        CHECK(ref.bundle.railFraction.size() == (spec.lengthSamples + 127) / 128);
        for (double r : ref.bundle.railFraction) CHECK(std::isfinite(r) && r > 0.5 && r < 1.1);
        const auto planA = randomPlan(spec, 1, 700);
        const auto planB = randomPlan(spec, 2, 4096);
        std::vector<FeedStep> planC;   // reversed member order, 1-frame blocks for member 0
        for (std::size_t i = members; i-- > 0;) planC.push_back({i, 333});
        for (int k = 0; k < 500; ++k) planC.push_back({0, 1});
        const auto a = coRender(spec, planA), b = coRender(spec, planB), c = coRender(spec, planC);
        CHECK(a.ok() && b.ok() && c.ok());
        const double eA = peakDiff(ref.bundle, a.bundle), eB = peakDiff(ref.bundle, b.bundle), eC = peakDiff(ref.bundle, c.bundle);
        CHECK(eA == 0.0 && eB == 0.0 && eC == 0.0);   // bit-identical
        const auto rA = replay(ref.bundle, spec, planA);
        const auto rB = replay(ref.bundle, spec, planB);
        CHECK(rA.valid() && rB.valid() && rA.peakError <= kReplayPeakTolerance && rB.peakError <= kReplayPeakTolerance);
        std::printf("  co-render %zu members: %zu/%zu host steps, replay peak error %.3g / %.3g / %.3g, max iterations %u\n",
                    members, planA.size(), planB.size(), eA, eB, eC, ref.maxIterationsUsed);
    }
}

void mismatchInvalidatesReplay() {
    const auto spec = makeSpec(3, 4000, 5);
    const auto ref = coRender(spec);
    CHECK(ref.ok());
    auto expect = [&](const CoRenderSpec& s, ReplayError e) {
        const auto r = replay(ref.bundle, s);
        CHECK(r.error == e && !r.reason.empty());
        std::printf("  replay invalid: %s\n", r.reason.c_str());
    };
    { auto s = spec; s.members[1].audio[0][1234] += 1e-9; expect(s, ReplayError::ContentHashMismatch); }
    { auto s = spec; s.members[2].activity[3] = 0.25; expect(s, ReplayError::AutomationHashMismatch); }
    { auto s = spec; s.startSample += 1; expect(s, ReplayError::StartMismatch); }
    { auto s = spec; s.bleed.levelDb = -40; expect(s, ReplayError::RoutingMismatch); }
    { auto s = spec; s.members[0].bleedOptIn = false; expect(s, ReplayError::RoutingMismatch); }
    { auto s = spec; s.members.pop_back(); expect(s, ReplayError::RoutingMismatch); }
    { auto s = spec; s.bleed.seed += 1; expect(s, ReplayError::SeedMismatch); }
    { auto s = spec; s.psu.capacitanceF *= 2; expect(s, ReplayError::ModelMismatch); }
    { auto s = spec; s.sampleRate = 44100; expect(s, ReplayError::SampleRateMismatch); }
    { auto s = spec; s.lengthSamples -= 1; for (auto& m : s.members) for (auto& ch : m.audio) ch.pop_back(); expect(s, ReplayError::LengthMismatch); }
    { auto bad = ref.bundle; bad.bleed[0][0][10] += 1e-3; CHECK(replay(bad, spec).error == ReplayError::StreamMismatch); }
    { auto bad = ref.bundle; bad.manifest.formatVersion = 2; CHECK(replay(bad, spec).error == ReplayError::FormatVersion); }
    { auto bad = ref.bundle; bad.manifest.bleedModelVersion = "x"; CHECK(replay(bad, spec).error == ReplayError::ModelMismatch); }
    CHECK(replay(ref.bundle, spec).valid());
}

void unconvergedRejected() {
    auto spec = makeSpec(4, 2048, 9);
    spec.psu.sourceResistanceOhm = 20000;   // |dF/dr| >> 1: plain iteration diverges
    for (auto& m : spec.members) m.quiescentA = 0.5;
    spec.maxIterations = 2;                 // too few evaluations for the secant to settle
    const auto r = coRender(spec);
    CHECK(r.error == CoRenderError::Unconverged && r.bundle.railFraction.empty());
    std::printf("  rejected: %s\n", r.reason.c_str());
    auto bad = makeSpec(2, 256, 1);
    bad.maxIterations = 9;
    CHECK(coRender(bad).error == CoRenderError::InvalidSpec);
    bad = makeSpec(2, 256, 1);
    bad.members[1].member = bad.members[0].member;
    CHECK(coRender(bad).error == CoRenderError::DuplicateMember);
    bad = makeSpec(2, 256, 1);
    bad.bleed.levelDb = -10;
    CHECK(coRender(bad).error == CoRenderError::InvalidBleed);
    bad = makeSpec(2, 256, 1);
    bad.members[0].audio[0].resize(10);
    CHECK(coRender(bad).error == CoRenderError::BadFeed);
    CoRenderSession partial(makeSpec(2, 256, 1));
    CHECK(partial.finish().error == CoRenderError::IncompleteFeed);
}

// Impulse into member A only: A's bleed stream stays zero (no own-bleed recursion), B gets W_BA.
void noFeedbackInCoRender() {
    CoRenderSpec s = makeSpec(2, 1024, 3);
    for (auto& m : s.members) { m.channels = 2; m.audio.assign(2, std::vector<double>(1024, 0.0)); }
    s.members[0].audio[0][300] = 1.0;
    s.bleed.levelDb = -24;
    const auto r = coRender(s);
    CHECK(r.ok());
    const auto& mf = r.bundle.manifest;
    const std::size_t ia = mf.memberOrder[0] == s.members[0].member ? 0 : 1, ib = 1 - ia;
    for (const auto& ch : r.bundle.bleed[ia]) for (double v : ch) CHECK(v == 0.0);
    const double w = std::pow(10.0, -24 / 20.0) * bleedSign(s.bleed.seed, s.members[0].member);
    for (std::size_t t = 0; t < 1024; ++t) CHECK(r.bundle.bleed[ib][0][t] == (t == 300 ? w : 0.0) && r.bundle.bleed[ib][1][t] == 0.0);
}

// Measured on white-noise members: per-receiver weights estimated by cross-correlation, and the
// hard bound |c_i| <= g * max|x|, for the default law and an explicit topology matrix.
void measuredRowSums() {
    constexpr std::size_t n = 8, len = 1 << 16;
    const double g = std::pow(10.0, -24 / 20.0);
    std::vector<Uuid128> ids;
    for (std::size_t i = 0; i < n; ++i) ids.push_back({0xAB00 + i, i});
    BleedConfig def;
    def.levelDb = -24;
    def.seed = 1;
    BleedConfig mat = def;
    BleedTopology t;
    t.members = ids;
    t.weights.assign(n * n, 0.0);
    std::mt19937_64 rng(77);
    std::uniform_real_distribution<double> u(-1, 1);
    for (std::size_t i = 0; i < n; ++i) {   // random signed row scaled to exactly g
        double sum = 0;
        for (std::size_t j = 0; j < n; ++j) if (i != j) sum += std::fabs(t.weights[i * n + j] = u(rng));
        for (std::size_t j = 0; j < n; ++j) t.weights[i * n + j] *= g / sum;
    }
    mat.topology = t;
    std::vector<std::vector<double>> x(n, std::vector<double>(len)), c(n, std::vector<double>(len));
    for (auto& v : x) for (double& s : v) s = u(rng);
    for (const BleedConfig* cfg : {&def, &mat}) {
        std::vector<double> W;
        CHECK(buildBleedWeights(*cfg, ids, W) == BleedError::None);
        std::vector<BleedSource> src(n);
        std::vector<BleedSink> snk(n);
        long viol;
        {
            cd::test::RtTrapScope trap;
            for (std::size_t off = 0; off < len; off += 128) {
                for (std::size_t i = 0; i < n; ++i) {
                    src[i].ch[0] = x[i].data() + off; src[i].channels = 1;
                    snk[i].ch[0] = c[i].data() + off; snk[i].channels = 1;
                }
                mixBleed(W, n, src, snk, 128);
            }
            viol = trap.counts().violations();
        }
        CHECK(viol == 0);
        // Least-squares identification of every receiver's weights from the noise streams:
        // solve (X^T X) w_i = X^T c_i (Gaussian elimination, partial pivoting).
        std::vector<double> G(n * n, 0.0);
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = 0; b < n; ++b)
                for (std::size_t s = 0; s < len; ++s) G[a * n + b] += x[a][s] * x[b][s];
        double worstRow = 0, worstPeak = 0;
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<double> A = G, rhs(n, 0.0);
            for (std::size_t a = 0; a < n; ++a)
                for (std::size_t s = 0; s < len; ++s) rhs[a] += x[a][s] * c[i][s];
            for (std::size_t k = 0; k < n; ++k) {
                std::size_t piv = k;
                for (std::size_t r = k + 1; r < n; ++r) if (std::fabs(A[r * n + k]) > std::fabs(A[piv * n + k])) piv = r;
                for (std::size_t q = 0; q < n; ++q) std::swap(A[k * n + q], A[piv * n + q]);
                std::swap(rhs[k], rhs[piv]);
                for (std::size_t r = k + 1; r < n; ++r) {
                    const double f = A[r * n + k] / A[k * n + k];
                    for (std::size_t q = k; q < n; ++q) A[r * n + q] -= f * A[k * n + q];
                    rhs[r] -= f * rhs[k];
                }
            }
            std::vector<double> w(n, 0.0);
            for (std::size_t k = n; k-- > 0;) {
                double acc = rhs[k];
                for (std::size_t q = k + 1; q < n; ++q) acc -= A[k * n + q] * w[q];
                w[k] = acc / A[k * n + k];
            }
            double row = 0, peak = 0;
            for (std::size_t j = 0; j < n; ++j) {
                row += std::fabs(w[j]);
                CHECK(std::fabs(w[j] - W[i * n + j]) < 1e-12);   // identification recovers the law
            }
            for (double v : c[i]) peak = std::max(peak, std::fabs(v));
            worstRow = std::max(worstRow, row);
            worstPeak = std::max(worstPeak, peak);
        }
        CHECK(maxAbsRowSum(W, n) <= g * (1 + 1e-12));
        CHECK(worstPeak <= g * (1 + 1e-12));
        CHECK(worstRow <= g * (1 + 1e-9));
        std::printf("  %s topology: exact max row sum %.6g, measured %.6g (measured/g - 1 = %.2e), peak |c|/max|x| %.4f g\n",
                    cfg->topology ? "matrix" : "default", maxAbsRowSum(W, n), worstRow, worstRow / g - 1, worstPeak / g);
    }
    BleedConfig over = mat;
    over.topology->weights[1] *= 1.01;
    CHECK(validateBleed(over) == BleedError::TopologyRowSumExceedsGain);
    std::vector<double> W;
    CHECK(buildBleedWeights(over, ids, W) == BleedError::TopologyRowSumExceedsGain);
}

void bundleReader() {
    const auto spec = makeSpec(3, 3000, 21);
    const auto ref = coRender(spec);
    CHECK(ref.ok());
    BundleReader rd;
    const auto& m = spec.members[2];
    const InputDigest own = digestInput(m, spec.lengthSamples);
    CHECK(rd.open(ref.bundle, own, spec.sampleRate, spec.startSample).valid());
    std::size_t idx = 0;
    for (std::size_t i = 0; i < ref.bundle.manifest.memberOrder.size(); ++i) if (ref.bundle.manifest.memberOrder[i] == m.member) idx = i;
    std::vector<double> out[2] = {std::vector<double>(3100), std::vector<double>(3100)};
    double* op[2] = {out[0].data(), out[1].data()};
    double rail = 0;
    long viol;
    {
        cd::test::RtTrapScope trap;
        std::uint64_t pos = spec.startSample - 50;
        std::size_t done = 0;
        for (std::uint32_t blk : {50u, 1u, 127u, 129u, 900u, 1893u}) {
            double* p[2] = {op[0] + done, op[1] + done};
            rail = rd.read(pos, blk, p, m.channels);
            pos += blk;
            done += blk;
        }
        viol = trap.counts().violations();
    }
    CHECK(viol == 0);
    bool same = true;
    for (std::uint32_t c = 0; c < m.channels; ++c)
        for (std::size_t s = 0; s < 3100; ++s) {
            const double want = (s >= 50 && s - 50 < 3000) ? ref.bundle.bleed[idx][c][s - 50] : 0.0;
            same = same && out[c][s] == want;
        }
    CHECK(same && rail == ref.bundle.railFraction[(1207 - 50) / 128]);   // quantum of the last block start
    double* none[2] = {op[0], op[1]};
    CHECK(rd.read(spec.startSample + 3000, 4, none, m.channels) == 1.0 && out[0][0] == 0.0);   // past the bundle
    InputDigest wrong = own;
    wrong.content[0] ^= 1;
    CHECK(rd.open(ref.bundle, wrong, spec.sampleRate, spec.startSample).error == ReplayError::ContentHashMismatch && !rd.isOpen());
    CHECK(rd.open(ref.bundle, own, spec.sampleRate, spec.startSample + 128).error == ReplayError::StartMismatch);
    InputDigest stranger = own;
    stranger.member = {1, 1};
    CHECK(rd.open(ref.bundle, stranger, spec.sampleRate, spec.startSample).error == ReplayError::UnknownMember);
}

}  // namespace

int main() {
    replayIndependentOfHostOrder();
    mismatchInvalidatesReplay();
    unconvergedRejected();
    noFeedbackInCoRender();
    measuredRowSums();
    bundleReader();
    std::printf("network co-render T11/P16: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
