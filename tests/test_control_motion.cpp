// Creative motion tests: seeded OU stream and pitch-drift delay. Creative process, not physical temperature.
#include "control/CreativeMotion.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using namespace cd::control;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)
const double kFs = 48000;

std::vector<double> stream(uint64_t seed, std::size_t bs, std::size_t N = 96000 * 3) {
    CreativeMotion m; m.prepare(kFs, seed);
    std::vector<double> o;
    for (std::size_t n = 0; n < N;) {
        const std::size_t len = std::min(bs, N - n);
        for (std::size_t i = 0; i < len; ++i, ++n) { m.advanceSample(); o.push_back(m.q()); }
    }
    return o;
}

void motionStreams() {
    const auto ref = stream(42, 1);
    for (std::size_t bs : {17u, 64u, 127u, 512u, 8192u}) CHECK(stream(42, bs) == ref);  // identical across partitions
    const auto other = stream(43, 64);
    CHECK(other != ref);
    double mx = 0, sum = 0, sum2 = 0; std::size_t changes = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        mx = std::max(mx, std::fabs(ref[i])); sum += ref[i]; sum2 += ref[i] * ref[i];
        if (i && ref[i] != ref[i - 1]) ++changes;
    }
    CHECK(mx < 1.0 && std::isfinite(sum));
    CHECK(changes >= 590 && changes <= 601);  // 6 s at 100 Hz
    std::printf("[motion] seed 42: max|q|=%.3f rms=%.3f control-tick changes=%zu\n", mx, std::sqrt(sum2 / ref.size()), changes);
    CHECK(deriveSeed(1, 0, "abc", StreamType::CutoffMotion) == deriveSeed(1, 0, "abc", StreamType::CutoffMotion));
    CHECK(deriveSeed(1, 0, "abc", StreamType::CutoffMotion) != deriveSeed(1, 1, "abc", StreamType::CutoffMotion));
    CHECK(deriveSeed(1, 0, "abc", StreamType::CutoffMotion) != deriveSeed(1, 0, "abd", StreamType::CutoffMotion));
    CHECK(deriveSeed(1, 0, "abc", StreamType::CutoffMotion) != deriveSeed(1, 0, "abc", StreamType::DelayMotion));
    CHECK(deriveSeed(1, 0, "abc", StreamType::Other) != deriveSeed(2, 0, "abc", StreamType::Other));
    Xoshiro256ss r; r.seed(7);  // gaussian sanity
    double m1 = 0, v = 0; const int K = 200000;
    for (int i = 0; i < K; ++i) { const double g = r.gaussian(); m1 += g; v += g * g; }
    CHECK(std::fabs(m1 / K) < 0.02 && std::fabs(v / K - 1) < 0.03);
    CreativeMotion bad; CHECK(!bad.prepare(0.0, 1) && !bad.prepare(std::numeric_limits<double>::quiet_NaN(), 1));
}

void delay() {
    PitchDriftDelay dl; CHECK(dl.prepare(kFs));
    const double base = 1.5e-3 * kFs;
    CHECK(dl.latencySamples() == base);
    // zero d -> constant delay and exact delayed-impulse response
    std::vector<double> y;
    for (int i = 0; i < 400; ++i) y.push_back(dl.process(i == 10 ? 1.0 : 0.0, 0.0, 1.0));
    CHECK(dl.currentDelaySamples() == base);
    double s = 0; for (double v : y) s += v;
    CHECK(std::fabs(s - 1.0) < 1e-12);  // Lagrange partition of unity
    // moving: alternate q=+1/-1 with d=1, check bound and slew
    PitchDriftDelay d2; d2.prepare(kFs);
    double prev = d2.currentDelaySamples(), lo = prev, hi = prev, maxStep = 0;
    CreativeMotion m; m.prepare(kFs, 5);
    for (int i = 0; i < 48000 * 30; ++i) {
        m.advanceSample();
        const double q = (i / 96000) % 2 ? -1.0 : 1.0;  // stress: full swing every 2 s
        d2.process(std::sin(0.01 * i), 1.0, q);
        const double c = d2.currentDelaySamples();
        maxStep = std::max(maxStep, std::fabs(c - prev)); lo = std::min(lo, c); hi = std::max(hi, c); prev = c;
    }
    std::printf("[delay] delay range %.3f..%.3f ms, max slew %.6f samples/sample\n", lo / kFs * 1e3, hi / kFs * 1e3, maxStep);
    CHECK(maxStep <= 0.002 + 1e-12);                  // 0.002 s/s
    CHECK(hi <= (1.5 + 1.5) * 1e-3 * kFs + 1e-9);     // +1.5 ms bound
    CHECK(lo >= 1.0 - 1e-12 && lo >= (1.5 - 1.5) * 1e-3 * kFs - 1e-9);  // lower bound (>= 1 sample for 4-point interp)
    CHECK(std::fabs(lo - 1.0) < 1e-9 || lo >= 0);
    // NaN safe; disabled passthrough with zero latency
    CHECK(std::isfinite(d2.process(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(), 5.0)));
    d2.setEnabled(false);
    CHECK(d2.process(0.25, 1, 1) == 0.25 && d2.latencySamples() == 0.0);
    PitchDriftDelay bad; CHECK(!bad.prepare(-1.0) && bad.process(0.5, 0, 0) == 0.5);
}

}  // namespace

int main() {
    motionStreams(); delay();
    std::puts(failures ? "FAILED" : "control motion ok");
    return failures ? 1 : 0;
}
