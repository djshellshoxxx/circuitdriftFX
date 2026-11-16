// UX-04 / UX-05 / UX-06 display-math tests (P09). All fixtures are SYNTHETIC (analytic tones,
// deterministic noise, an exponential rail recovery); nothing here is a physical validation and
// the G01 calibration gates remain open.
#include "analysis/PhaseCoherence.h"
#include "analysis/RailDisplay.h"
#include "analysis/ScopeMath.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace cd::analysis;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
constexpr double kPi = 3.14159265358979323846;
const double kNan = std::numeric_limits<double>::quiet_NaN();

// ---------------- UX-04 ----------------
void scopeMapping() {
    const XyMapper m(512, 512, 1.0 / 128.0);  // +-2 V full scale, equal volts/pixel on both axes
    CHECK(m.valid());
    double worstAxis = 0, worstDiag = 0;
    for (int i = -240; i <= 240; ++i) {
        const double v = double(float(i) * 0.0078125f * 0.9f);  // float telemetry values
        const PixelPoint e = m.map(v, v);
        const PixelPoint q = m.mapRounded(v, v);
        worstDiag = std::max(worstDiag, std::fabs((e.x - 256.0) + (e.y - 256.0)));  // exact 45-degree line
        worstAxis = std::max({worstAxis, std::fabs(q.x - e.x), std::fabs(q.y - e.y)});
    }
    std::printf("UX-04 unity diagonal: exact-mapping deviation %.3g px, raster error per axis %.3f px\n", worstDiag,
                worstAxis);
    CHECK(worstDiag < 1e-9);
    CHECK(worstAxis <= 0.5);

    std::vector<PixelPoint> pts;
    for (int i = -200; i <= 200; ++i) {
        const double x = i * 0.01;
        pts.push_back(m.mapRounded(x, 0.5 * x));
    }
    const double slope = fitSlope(pts.data(), pts.size());
    std::printf("UX-04 0.5-gain slope %.5f\n", slope);
    CHECK(std::fabs(slope - 0.5) < 0.005);  // within 1% of 0.5

    CHECK(!XyMapper(0, 10, 1.0).valid() && !XyMapper(10, 10, 0.0).valid() && !XyMapper(10, 10, kNan).valid());
    const PixelPoint bad = m.map(kNan, 0);
    CHECK(std::isnan(bad.x) && !m.onScreen(bad));
    CHECK(m.onScreen(m.map(0, 0)) && !m.onScreen(m.map(100, 0)));
    CHECK(std::isnan(fitSlope(nullptr, 0)));
}

void scopeModes() {
    ScopeModeInfo a = scopeModeInfo(ScopeMode::TransferXY, false);
    CHECK(a.available && std::string(a.xLabel) == "Input (V)" && std::string(a.yLabel) == "Output (V)");
    ScopeModeInfo t = scopeModeInfo(ScopeMode::VoltageTemperature, false);
    CHECK(t.available && std::string(t.yLabel).find("C)") != std::string::npos &&
          std::string(t.xLabel).find("(V)") != std::string::npos);  // separate units, never summed
    CHECK(std::string(scopeModeInfo(ScopeMode::VoltageRail, false, true).yLabel).find("fraction") != std::string::npos);
    ScopeModeInfo bh = scopeModeInfo(ScopeMode::MagneticBH, false);
    CHECK(!bh.available && std::string(bh.unavailableReason) == "model does not expose magnetic state");
    ScopeModeInfo bh2 = scopeModeInfo(ScopeMode::MagneticBH, true);
    CHECK(bh2.available && std::string(bh2.xLabel) == "H (A/m)" && std::string(bh2.yLabel) == "B (T)");
}

void scopeDecimation() {
    const std::size_t n = 100000;
    std::vector<XyPair> in(n), out(kMaxScopeTracePoints);
    for (std::size_t i = 0; i < n; ++i) {
        in[i].x = float(i);
        in[i].y = float(std::sin(2 * kPi * double(i) / 997.0));
    }
    in[54321].y = 5.0f;   // isolated extrema must survive
    in[12345].y = -5.0f;
    const std::size_t w = decimateExtrema(in.data(), n, out.data(), kMaxScopeTracePoints);
    CHECK(w > 0 && w <= kMaxScopeTracePoints);
    bool ordered = true, hasMax = false, hasMin = false;
    for (std::size_t i = 0; i < w; ++i) {
        if (i && out[i].x < out[i - 1].x) ordered = false;
        hasMax = hasMax || out[i].y == 5.0f;
        hasMin = hasMin || out[i].y == -5.0f;
    }
    CHECK(ordered && hasMax && hasMin);
    // small input copied unchanged; non-finite skipped; degenerate inputs safe
    std::vector<XyPair> small(10);
    for (int i = 0; i < 10; ++i) small[i] = {float(i), float(i)};
    small[3].y = std::numeric_limits<float>::quiet_NaN();
    CHECK(decimateExtrema(small.data(), 10, out.data(), 2048) == 9);
    CHECK(decimateExtrema(nullptr, 5, out.data(), 2048) == 0 && decimateExtrema(small.data(), 0, out.data(), 2048) == 0);
    CHECK(decimateExtrema(in.data(), n, out.data(), 0) <= 2);  // maxOut clamped to 2
}

// ---------------- UX-05 ----------------
// Synthetic fixture: pre-event 120 V DC rail, instantaneous droop of D volts at t0 (ms), then
// exponential recovery whose 95% point is exactly 50 ms after t0 (tau = 50/ln 20).
double railFixture(double t, double t0, double D, double reco95Ms) {
    if (t < t0) return 120.0;
    const double tau = reco95Ms / std::log(20.0);
    return 120.0 - D * std::exp(-(t - t0) / tau);
}
void fillRail(RailHistory& h, double t0, int nbins, int skipLo = -1, int skipHi = -1) {
    for (int b = 0; b < nbins; ++b) {
        if (b >= skipLo && b <= skipHi) continue;
        const float v = float(railFixture(double(b), t0, 28.0, 50.0));
        h.push(b, v, v, v);
    }
}

void railRecovery() {
    for (double t0 : {300.0, 300.4, 300.9}) {
        RailHistory h;
        fillRail(h, t0, 800);
        RecoveryParams p;
        p.startBin = 0; p.endBin = 799; p.targetRecoveryMs = 50.0;
        const RecoveryResult r = estimateRecovery(h, p);
        CHECK(r.status == RecoveryStatus::Ok);
        const double err = std::fabs(r.measuredRecoveryMs - 50.0);
        std::printf("UX-05 recovery t0=%.1f measured %.3f ms (err %.3f ms), pre %.2f V, min %.2f V\n", t0,
                    r.measuredRecoveryMs, err, r.preLevelV, r.minRailV);
        CHECK(err <= 2.0);
        CHECK(std::fabs(r.preLevelV - 120.0) < 1e-4);
        CHECK(r.minRailV < 120.0 - 20.0);
        CHECK(r.targetRecoveryMs == 50.0);  // separate from the measurement
    }
    {   // not recovered: window ends early
        RailHistory h;
        fillRail(h, 300.0, 330);
        RecoveryParams p; p.startBin = 0; p.endBin = 329;
        CHECK(estimateRecovery(h, p).status == RecoveryStatus::NotRecovered);
    }
    {   // no event
        RailHistory h;
        fillRail(h, 5000.0, 600);
        RecoveryParams p; p.startBin = 0; p.endBin = 599;
        CHECK(estimateRecovery(h, p).status == RecoveryStatus::NoEvent);
    }
    {   // overlapping event: second droop during recovery is rejected, not mis-measured
        RailHistory h;
        for (int b = 0; b < 800; ++b) {
            double v = railFixture(b, 300.0, 28.0, 50.0);
            if (b >= 330) v -= 20.0 * std::exp(-(b - 330) / 10.0) * (1.0 - std::exp(-(b - 329) / 1.0)) ;
            const float f = float(v);
            h.push(b, f, f, f);
        }
        RecoveryParams p; p.startBin = 0; p.endBin = 799;
        const RecoveryResult r = estimateRecovery(h, p);
        CHECK(r.status == RecoveryStatus::RejectedOverlap);
        CHECK(std::string(recoveryStatusLabel(r.status)).find("overlapping") != std::string::npos);
    }
    {   // gap inside the window: insufficient data, never a fabricated duration
        RailHistory h;
        fillRail(h, 300.0, 800, 320, 330);
        RecoveryParams p; p.startBin = 0; p.endBin = 799;
        CHECK(estimateRecovery(h, p).status == RecoveryStatus::InsufficientData);
    }
    {   // bad params
        RailHistory h;
        RecoveryParams p; p.startBin = 10; p.endBin = 10;
        CHECK(estimateRecovery(h, p).status == RecoveryStatus::InsufficientData);
        p.endBin = 200; p.recoverFraction = 1.5;
        CHECK(estimateRecovery(h, p).status == RecoveryStatus::InsufficientData);
    }
}

void railEnvelopeAndLabels() {
    RailHistory h;
    CHECK(h.newestBin() == -1);
    CHECK(!h.push(-1, 0, 0, 0) && !h.push(0, kNan, 0, 0) && !h.push(0, 2, 1, 3));  // rejects bad bins
    fillRail(h, 100000.0, 3000);  // constant 120 V
    const float spikeLo = 80.0f, spikeHi = 140.0f;
    h.push(1500, spikeLo, 120.0f, spikeHi);  // 1 ms excursion must survive decimation
    std::vector<EnvelopeColumn> col(100);
    EnvelopeInfo ei = h.envelope(h.newestBin(), 2000.0, col.data(), col.size(), h.newestBin());
    CHECK(!ei.empty && !ei.stale);
    float lo = 1e9f, hi = -1e9f;
    bool anyGap = false;
    for (auto& c : col) { CHECK(c.hasData); lo = std::min(lo, c.minV); hi = std::max(hi, c.maxV); anyGap = anyGap || c.hasGap; }
    CHECK(lo == spikeLo && hi == spikeHi && !anyGap);
    // 10 s retention: bin 0 is overwritten once 10000 later bins arrive
    RailBin rb;
    CHECK(h.get(5, rb));
    for (int b = 3000; b < 10100; ++b) h.push(b, 120, 120, 120);
    CHECK(!h.get(5, rb) && h.get(10099, rb));
    // gaps and stale
    h.clear();
    for (int b = 0; b < 1000; ++b) if (b < 400 || b > 409) h.push(b, 1, 1, 1);
    ei = h.envelope(999, 1000.0, col.data(), col.size(), 999);
    int gapCols = 0;
    for (auto& c : col) gapCols += c.hasGap ? 1 : 0;
    CHECK(gapCols >= 1 && gapCols <= 2);
    ei = h.envelope(999, 1000.0, col.data(), col.size(), 999 + 251);
    CHECK(ei.stale);
    ei = h.envelope(999, 1000.0, col.data(), col.size(), 999 + 250);
    CHECK(!ei.stale);
    CHECK(h.envelope(999, 1000.0, nullptr, 0, 999).empty);
    // AC vs DC labels are distinct
    CHECK(std::string(railKindLabel(RailKind::InternalDc)) != railKindLabel(RailKind::AcMainsInput));
    CHECK(std::string(railKindUnits(RailKind::InternalDc)) != railKindUnits(RailKind::AcMainsInput));
}

// ---------------- UX-06 ----------------
struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    double next() {  // approx N(0,1): sum of 12 uniforms - 6
        double a = 0;
        for (int i = 0; i < 12; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            a += double(s >> 11) / double(1ULL << 53);
        }
        return a - 6.0;
    }
};
double wrapPi(double x) {
    while (x > kPi) x -= 2 * kPi;
    while (x < -kPi) x += 2 * kPi;
    return x;
}

void fftChecks() {
    const std::size_t n = 16;
    Fft fft(n);
    std::vector<std::complex<double>> a(n), ref(n);
    Lcg g(5);
    for (auto& v : a) v = {g.next(), g.next()};
    for (std::size_t k = 0; k < n; ++k) {
        ref[k] = 0;
        for (std::size_t t = 0; t < n; ++t) ref[k] += a[t] * std::polar(1.0, -2 * kPi * double(k * t) / double(n));
    }
    fft.forward(a.data());
    double err = 0;
    for (std::size_t k = 0; k < n; ++k) err = std::max(err, std::abs(a[k] - ref[k]));
    CHECK(err < 1e-12);
    bool threw = false;
    try { Fft bad(12); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void phaseDelayTones() {
    for (double tau : {0.0, 0.25}) {
        PhaseConfig cfg;
        cfg.tauSeconds = tau;
        StereoPhaseAnalyzer an(cfg);
        const double fs = cfg.sampleRate;
        const double delay = 0.00031;  // 0.31 ms, non-integer samples (analytic delay)
        const std::size_t kBins[] = {20, 64, 150, 400, 900};
        std::vector<float> L(60000), R(60000);
        for (std::size_t i = 0; i < L.size(); ++i) {
            double l = 0, r = 0;
            for (std::size_t k : kBins) {
                const double f = double(k) * fs / 2048.0;
                l += 0.1 * std::sin(2 * kPi * f * double(i) / fs);
                r += 0.1 * std::sin(2 * kPi * f * (double(i) / fs - delay));
            }
            L[i] = float(l); R[i] = float(r);
        }
        an.process(L.data(), R.data(), L.size());
        std::vector<PhaseBin> bins(an.binCount());
        const PhaseSnapshot s = an.snapshot(bins.data());
        CHECK(s.applicable && s.settled && s.frames > 50);
        double worst = 0;
        for (std::size_t k : kBins) {
            const PhaseBin& b = bins[k - 1];
            const double expect = 2 * kPi * b.freqHz * delay;  // positive: R lags L
            worst = std::max(worst, std::fabs(wrapPi(b.phaseRad - expect)) * 180.0 / kPi);
            CHECK(b.coherence > 0.99 && b.visible);
        }
        std::printf("UX-06 tone delay (tau=%.2f): worst phase error %.4f deg\n", tau, worst);
        CHECK(worst <= 3.0);
        CHECK(std::fabs(StereoPhaseAnalyzer::polarRadius(20, 20000)) < 1e-12);
        CHECK(std::fabs(StereoPhaseAnalyzer::polarRadius(20000, 20000) - 1.0) < 1e-12);
        CHECK(StereoPhaseAnalyzer::polarRadius(5, 20000) == 0.0 && StereoPhaseAnalyzer::polarRadius(1e9, 20000) == 1.0);
        CHECK(StereoPhaseAnalyzer::polarRadius(kNan, 20000) == 0.0);
    }
}

void phaseDelayNoiseAndIndependent() {
    PhaseConfig cfg;
    cfg.tauSeconds = 0.0;  // Welch
    StereoPhaseAnalyzer coh(cfg), ind(cfg);
    const int d = 8;  // samples
    std::vector<float> L(120000), R(120000), N1(120000), N2(120000);
    Lcg g1(1), g2(2), g3(3);
    for (std::size_t i = 0; i < L.size(); ++i) {
        L[i] = float(0.1 * g1.next());
        N1[i] = float(0.1 * g2.next());
        N2[i] = float(0.1 * g3.next());
    }
    for (std::size_t i = 0; i < R.size(); ++i) R[i] = i >= std::size_t(d) ? L[i - d] : 0.0f;
    coh.process(L.data(), R.data(), L.size());
    ind.process(N1.data(), N2.data(), N1.size());
    std::vector<PhaseBin> a(coh.binCount()), b(ind.binCount());
    coh.snapshot(a.data());
    const PhaseSnapshot si = ind.snapshot(b.data());
    double worst = 0, meanCoh = 0, meanInd = 0;
    std::size_t counted = 0;
    for (std::size_t k = 4; k < a.size() - 4; ++k) {
        if (a[k].coherence > 0.9) {
            ++counted;
            const double expect = 2 * kPi * a[k].freqHz * d / cfg.sampleRate;
            worst = std::max(worst, std::fabs(wrapPi(a[k].phaseRad - expect)) * 180.0 / kPi);
        }
        meanCoh += a[k].coherence;
        meanInd += b[k].coherence;
    }
    const double nb = double(a.size() - 8);
    meanCoh /= nb; meanInd /= nb;
    std::printf("UX-06 noise: coherent bins %zu/%zu worst phase err %.3f deg, mean coherence %.3f; independent mean %.4f (%zu frames)\n",
                counted, a.size() - 8, worst, meanCoh, meanInd, si.frames);
    CHECK(counted > (a.size() - 8) * 9 / 10);
    CHECK(worst <= 3.0);
    CHECK(meanCoh > 0.95);
    CHECK(si.settled && meanInd < 0.1);  // stated threshold: mean gamma^2 < 0.1 over >=100 averaged frames
    for (auto& x : b) CHECK(x.coherence >= 0.0 && x.coherence <= 1.0);
}

void phaseEdgeCases() {
    PhaseConfig cfg;
    StereoPhaseAnalyzer an(cfg);
    std::vector<PhaseBin> bins(an.binCount());
    // nothing yet / mono -> not applicable
    CHECK(!an.snapshot(bins.data()).applicable);
    std::vector<float> x(8192, 0.25f);
    an.process(x.data(), nullptr, x.size());
    PhaseSnapshot s = an.snapshot(bins.data());
    CHECK(!s.applicable && !s.settled && bins[10].coherence == 0.0 && !bins[10].visible);
    // silence stereo: no NaN, nothing visible
    std::vector<float> z(20000, 0.0f);
    an.process(z.data(), z.data(), z.size());   // switching mono->stereo resets averages
    s = an.snapshot(bins.data());
    bool finite = true, anyVisible = false;
    for (auto& b : bins) { finite = finite && std::isfinite(b.phaseRad) && std::isfinite(b.magnitudeDbfs) && std::isfinite(b.coherence); anyVisible = anyVisible || b.visible; }
    CHECK(s.applicable && finite && !anyVisible);
    // NaN/Inf input treated as zero
    an.reset();
    std::vector<float> bad(10000, kNan);
    bad[5] = std::numeric_limits<float>::infinity();
    an.process(bad.data(), bad.data(), bad.size());
    s = an.snapshot(bins.data());
    finite = true;
    for (auto& b : bins) finite = finite && std::isfinite(b.phaseRad) && std::isfinite(b.coherence) && b.coherence <= 1.0;
    CHECK(finite);
    an.process(nullptr, nullptr, 10);
    an.process(z.data(), z.data(), 0);
    CHECK(an.snapshot(nullptr).applicable == false);
    // invalid configs
    auto throws = [](PhaseConfig c) { try { StereoPhaseAnalyzer a(c); } catch (const std::invalid_argument&) { return true; } return false; };
    PhaseConfig c = cfg; c.fftSize = 1000; CHECK(throws(c));
    c = cfg; c.hop = 0; CHECK(throws(c));
    c = cfg; c.hop = 4096; CHECK(throws(c));
    c = cfg; c.sampleRate = -1; CHECK(throws(c));
    c = cfg; c.tauSeconds = kNan; CHECK(throws(c));
}

void phaseBlockPartition() {
    PhaseConfig cfg;
    std::vector<float> L(30000), R(30000);
    Lcg g(9);
    for (std::size_t i = 0; i < L.size(); ++i) { L[i] = float(0.1 * g.next()); R[i] = float(0.5 * L[i] + 0.05 * g.next()); }
    std::vector<PhaseBin> ref;
    for (std::size_t block : {std::size_t(8192), std::size_t(1), std::size_t(17), std::size_t(64), std::size_t(127), std::size_t(512)}) {
        StereoPhaseAnalyzer an(cfg);
        for (std::size_t o = 0; o < L.size(); o += block) {
            const std::size_t n = std::min(block, L.size() - o);
            an.process(L.data() + o, R.data() + o, n);
        }
        std::vector<PhaseBin> bins(an.binCount());
        an.snapshot(bins.data());
        if (ref.empty()) { ref = bins; continue; }
        bool same = true;
        for (std::size_t k = 0; k < bins.size(); ++k)
            same = same && bins[k].phaseRad == ref[k].phaseRad && bins[k].coherence == ref[k].coherence &&
                   bins[k].balance == ref[k].balance;
        CHECK(same);
    }
    // channel balance sign: R is quieter so balance > 0
    StereoPhaseAnalyzer an(cfg);
    an.process(L.data(), R.data(), L.size());
    std::vector<PhaseBin> bins(an.binCount());
    an.snapshot(bins.data());
    double mb = 0;
    for (auto& b : bins) mb += b.balance;
    CHECK(mb / double(bins.size()) > 0.3);
}

void dcAndCorrelation() {
    const double fs = 48000;
    StereoDcCorrelation dc(fs, 0.5);
    std::vector<float> L(60000), R(60000), M(60000);
    for (std::size_t i = 0; i < L.size(); ++i) {
        const double s = std::sin(2 * kPi * 100.0 * double(i) / fs);  // 50 whole cycles per 500 ms
        L[i] = float(0.1 + 0.5 * s);
        R[i] = float(0.03 + 0.5 * s);
    }
    dc.process(L.data(), R.data(), L.size());
    DcCorrelationReadout r = dc.readout();
    CHECK(r.applicable);
    CHECK(std::fabs(r.dcDifference - 0.07) < 1e-5 && std::fabs(r.meanL - 0.1) < 1e-5);
    CHECK(r.correlation > 0.9999);   // DC offset does not alter the correlation, nor vice versa
    for (std::size_t i = 0; i < R.size(); ++i) R[i] = -(L[i] - 0.1f);
    dc.reset();
    dc.process(L.data(), R.data(), L.size());
    r = dc.readout();
    CHECK(r.correlation < -0.9999 && std::fabs(r.dcDifference - 0.1) < 1e-5);
    Lcg g1(11), g2(12);
    std::vector<float> A(200000), B(200000);
    for (std::size_t i = 0; i < A.size(); ++i) { A[i] = float(0.1 * g1.next()); B[i] = float(0.1 * g2.next()); }
    dc.reset();
    dc.process(A.data(), B.data(), A.size());
    CHECK(std::fabs(dc.readout().correlation) < 0.05);
    // silence -> 0 correlation (not NaN); mono -> N/A; bad input tolerated
    dc.reset();
    std::vector<float> z(30000, 0.0f);
    dc.process(z.data(), z.data(), z.size());
    CHECK(dc.readout().applicable && dc.readout().correlation == 0.0);
    dc.reset();
    dc.process(L.data(), nullptr, L.size());
    CHECK(!dc.readout().applicable);
    z[7] = kNan;
    dc.process(z.data(), z.data(), z.size());
    CHECK(std::isfinite(dc.readout().correlation));
    bool threw = false;
    try { StereoDcCorrelation bad(0.0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
}  // namespace

int main() {
    scopeMapping();
    scopeModes();
    scopeDecimation();
    railRecovery();
    railEnvelopeAndLabels();
    fftChecks();
    phaseDelayTones();
    phaseDelayNoiseAndIndependent();
    phaseEdgeCases();
    phaseBlockPartition();
    dcAndCorrelation();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("analysis displays (UX-04/05/06): all checks passed\n");
    return 0;
}
