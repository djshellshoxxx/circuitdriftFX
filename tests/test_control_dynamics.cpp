// T15/T16 primitive-level tests. ALL fixtures here are SYNTHETIC (not hardware measurements);
// the reservoir is a toy C dV/dt = I_charge - I_load model, not a circuit solve. Integrated T15/T16 remain open.
#include "control/Dynamics.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using namespace cd::control;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)
const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kFs = 48000;

// Synthetic kick: silence then decaying 60 Hz burst.
std::vector<double> kick(std::size_t n, std::size_t onset) {
    std::vector<double> x(n, 0.0);
    for (std::size_t i = onset; i < n; ++i) {
        const double t = double(i - onset) / kFs;
        x[i] = 0.8 * std::exp(-t / 0.05) * std::sin(2 * M_PI * 60.0 * t);
    }
    return x;
}

void identity() {  // T15 amount 0 -> r_eff == r
    for (double r : {0.0, 0.3, 0.77, 1.0})
        for (double b : {0.0, 0.5, 1.0}) CHECK(std::fabs(TransientDetector::effectiveR(r, TransientDetector::strength(0.0, b)) - r) <= 1e-7);
    CHECK(TransientDetector::strength(kNan, 1.0) == 0.0);
}

void kickFixture() {  // T15 synthetic kick
    TransientDetector det; CHECK(det.prepare(kFs));
    const std::size_t onset = 4800, n = 24000;
    const auto x = kick(n, onset);
    std::vector<double> b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = det.process(x[i], 0.5 * x[i]);
    double peak1ms = 0;
    for (std::size_t i = onset; i < onset + 48 + 24; ++i) peak1ms = std::max(peak1ms, b[i]);  // ~1.5 ms window
    std::printf("[T15 synthetic] peak b in first 1.5 ms = %.3f\n", peak1ms);
    CHECK(peak1ms >= 0.9);
    CHECK(b[onset - 1] == 0.0);  // gated silence
    // Toy reservoir: C dV/dt = (Vn-V)/Rch - k|x|, with V normalised to 1 ; r = V.
    auto run = [&](double amount) {
        const double C = 0.01, Rch = 5.0, k = 1.2, dt = 1 / kFs;
        double V = 1.0, sumAtt = 0, peakAtt = 0; TransientDetector d; d.prepare(kFs);
        for (std::size_t i = 0; i < n; ++i) {
            const double bb = d.process(x[i], 0.5 * x[i]);
            V += dt / C * ((1.0 - V) / Rch - k * std::fabs(x[i]));  // physical r continues unchanged
            V = std::clamp(V, 0.0, 1.0);
            const double att = 1.0 - TransientDetector::effectiveR(V, TransientDetector::strength(amount, bb));
            if (i >= onset && i < onset + 480) { sumAtt += att; peakAtt = std::max(peakAtt, att); }  // first 10 ms
        }
        return std::pair<double, double>{sumAtt / 480, peakAtt};
    };
    const auto off = run(0.0), on = run(1.0);
    const double redMean = 1 - on.first / off.first, redPeak = 1 - on.second / off.second;
    std::printf("[T15 synthetic] 10 ms sag attenuation mean %.4f -> %.6f (reduced %.1f%%), peak %.4f -> %.6f (reduced %.1f%%)\n",
                off.first, on.first, 100 * redMean, off.second, on.second, 100 * redPeak);
    const auto half = run(0.5);
    std::printf("[T15 synthetic] amount 0.5: mean attenuation reduced %.1f%%\n", 100 * (1 - half.first / off.first));
    CHECK(redMean >= 0.5);
    CHECK(run(0.0).first == off.first);  // amount 0 deterministic identity
}

void nonFinite() {
    TransientDetector d; d.prepare(kFs);
    for (int i = 0; i < 100; ++i) { const double v = d.process(i % 3 ? kNan : std::numeric_limits<double>::infinity(), kNan); CHECK(std::isfinite(v)); }
    CHECK(d.sawNonFinite());
    CHECK(!d.prepare(0.0) && !d.prepare(kNan));
}

double settledI(SidechainLoadDetector& s, double amp, double hz, double sec) {
    double y = 0;
    for (std::size_t i = 0; i < std::size_t(sec * kFs); ++i) y = s.process(amp * std::sin(2 * M_PI * hz * i / kFs), 0.0);
    return y;
}

void sidechain() {  // T16 synthetic
    SidechainLoadDetector::Config c; c.depth = 1.0; c.iBudgetAmps = 0.5;
    SidechainLoadDetector s; CHECK(s.prepare(kFs, c));
    const double amp = std::pow(10.0, -18.0 / 20) * std::sqrt(2.0);  // -18 dBFS RMS sine
    const double i1 = settledI(s, amp, 1000, 1.0);
    std::printf("[T16 synthetic] -18 dBFS RMS 1 kHz sine: I_ext=%.6f A (target %.6f)\n", i1, 0.5);
    CHECK(std::fabs(i1 / 0.5 - 1.0) <= 0.01);
    c.depth = 2.0; s.prepare(kFs, c);  // depth scaling, and clamp at 4x
    CHECK(std::fabs(settledI(s, amp, 1000, 1.0) / 1.0 - 1.0) <= 0.01);
    CHECK(settledI(s, amp * 100, 1000, 0.5) <= 2.0 * 0.5 * 4.0 + 1e-9);
    c.depth = 0.0; s.prepare(kFs, c);  // depth 0 identity
    CHECK(settledI(s, amp, 1000, 0.5) == 0.0);
    c.depth = 1.0; s.prepare(kFs, c);  // disconnected
    s.setConnected(false);
    CHECK(settledI(s, amp, 1000, 0.2) == 0.0 && s.status().disconnected);
    s.setConnected(true);
    CHECK(!s.status().disconnected && settledI(s, 0.0, 1000, 0.5) < 1e-6);  // external silence
    c.source = SidechainLoadDetector::Source::Main; s.prepare(kFs, c);  // Main -> no external current
    CHECK(settledI(s, amp, 1000, 0.2) == 0.0 && !s.status().disconnected);
    // HPF removes 20 Hz rumble at default 40 Hz? (Butterworth: -12 dB/oct => ~ -12 dB at 20 Hz)
    c.source = SidechainLoadDetector::Source::External; s.prepare(kFs, c);
    CHECK(settledI(s, amp, 20, 1.0) < 0.5 * 0.5);
    // NaN input sanitized
    c.hpfHz = 1000; CHECK(s.prepare(kFs, c));  // clamped to 300
    double y = 0; for (int i = 0; i < 50; ++i) y = s.process(kNan, kNan);
    CHECK(std::isfinite(y) && s.status().sanitized);
    CHECK(!s.prepare(kNan, c) && !s.prepare(-1.0, c));
}

}  // namespace

int main() {
    identity(); kickFixture(); nonFinite(); sidechain();
    std::puts(failures ? "FAILED" : "control dynamics ok");
    return failures ? 1 : 0;
}
