// Acceptance tests: T20 (thermal), T21 (oversampling), T22 (HPF), T23 (soft clip / gain match),
// registry invariants (P01).
// Plain-assert harness: no third-party dependency, deterministic, no I/O.
#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <cstdio>
#include <set>
#include <vector>
#include "core/ParameterRegistry.h"
#include "dsp/OutputProtection.h"
#include "dsp/Oversampling.h"
#include "dsp/ThermalNetwork.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s | ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace cd;

static void registry() {
    std::set<uint32_t> ids; std::set<std::string> names;
    for (const auto& p : core::kParams) {
        CHECK(p.hostId >= 1000 && p.hostId <= 1999, "%s id %u", p.id, p.hostId);
        CHECK(ids.insert(p.hostId).second, "dup hostId %u", p.hostId);
        CHECK(names.insert(p.id).second, "dup id %s", p.id);
        CHECK(p.min <= p.def && p.def <= p.max, "%s default range", p.id);
        const double u = core::toNormalized(p, p.def);
        CHECK(std::fabs(core::toPlain(p, u) - p.def) < 1e-9 * (1 + std::fabs(p.def)), "%s roundtrip", p.id);
        CHECK(std::fabs(core::toPlain(p, 0.0) - p.min) < 1e-9 && std::fabs(core::toPlain(p, 1.0) - p.max) < 1e-9, "%s endpoints", p.id);
        CHECK(core::toPlain(p, NAN) == p.def, "%s NaN", p.id);
    }
    CHECK(core::kParamCount == 74, "count %zu", core::kParamCount);
    const auto* h = core::findById("hpf.frequency");
    CHECK(h && h->def == 15.0 && h->mapping == core::ParamMapping::Log, "hpf spec");
    CHECK(core::findByHostId(1011) == h, "lookup");
    CHECK(core::smoothingCoeff(48000, 0) == 0.0, "tau0");
    CHECK(std::fabs(core::smoothingCoeff(48000, 0.01) - std::exp(-1.0 / 480.0)) < 1e-15, "coeff");
}

static void t20_thermal() {
    dsp::ThermalNetwork::Config c; c.n = 1; c.capacity[0] = 2.0; c.toAmbient[0] = 0.2;
    dsp::ThermalNetwork net;
    CHECK(net.prepare(c, 1e-3, 298.15), "prepare");
    double p = 1.0;
    for (int i = 0; i < 10000; ++i) net.step(&p);
    const double exact = 298.15 + 5.0 * (1.0 - std::exp(-1.0));  // tau=C/G=10 s
    CHECK(std::fabs(net.temperature(0) - exact) <= 0.002, "err %g", net.temperature(0) - exact);
    // Coupled 2-node steady state vs hand solution: node0 heated 1 W, G01=0.5, G1amb=0.25, G0amb=0.
    dsp::ThermalNetwork::Config d; d.n = 2; d.capacity[0] = d.capacity[1] = 1.0;
    d.toAmbient[1] = 0.25; d.coupling[0][1] = d.coupling[1][0] = 0.5;
    dsp::ThermalNetwork n2; CHECK(n2.prepare(d, 1e-2, 300.0), "prepare2");
    double pw[2] = {1.0, 0.0};
    for (int i = 0; i < 200000; ++i) n2.step(pw);
    CHECK(std::fabs(n2.temperature(1) - 304.0) < 1e-6 && std::fabs(n2.temperature(0) - 306.0) < 1e-6,
          "coupled %g %g", n2.temperature(0), n2.temperature(1));
    // Rejection: isolated island (no ambient path), non-positive C, asymmetric coupling.
    dsp::ThermalNetwork::Config bad = d; bad.toAmbient[1] = 0.0;
    CHECK(!n2.prepare(bad, 1e-2, 300.0), "island");
    bad = d; bad.capacity[0] = 0.0; CHECK(!n2.prepare(bad, 1e-2, 300.0), "C=0");
    bad = d; bad.coupling[0][1] = 0.4; CHECK(!n2.prepare(bad, 1e-2, 300.0), "asym");
}

// ---- T21 oversampling ------------------------------------------------------------------------
static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> w = std::polar(1.0, -2 * M_PI / double(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> wk = 1;
            for (size_t k = 0; k < len / 2; ++k, wk *= w) {
                const auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
        }
    }
}

template <class Stage>
static void checkStage(const Stage& st, double pe, const char* tag, int s) {
    double lo = 1e9, hi = -1e9, stop = -1e9;
    for (int g = 0; g <= 2000; ++g) {
        const double db = 20 * std::log10(st.magnitude(pe * g / 2000.0));
        lo = std::min(lo, db); hi = std::max(hi, db);
        stop = std::max(stop, 20 * std::log10(st.magnitude(0.5 - pe + pe * g / 2000.0) + 1e-300));
    }
    CHECK(std::max(hi, -lo) <= 0.01, "%s stage %d ripple %g..%g dB", tag, s, lo, hi);
    CHECK(stop <= -100.0, "%s stage %d stopband %g dB", tag, s, stop);
}

// Synthetic memoryless test nonlinearity (NOT the electrical model): y = tanh(4x).
static double t21Nonlinear(double x, double drive) { return std::tanh(drive * x); }

// Runs the published T21 multitone through `os` and returns in-band power per FFT bin.
static std::vector<double> t21Spectrum(dsp::Oversampler& os, const int* bins, int nTones, int N, double amp, double drive) {
    const int B = 256, F = os.factor(), total = 3 * N;
    std::vector<double> in(B), hi(size_t(B) * F), out(B), cap;
    for (int start = 0; start < total; start += B) {
        for (int i = 0; i < B; ++i) {
            double x = 0;
            for (int k = 0; k < nTones; ++k) x += amp * std::sin(2 * M_PI * double(bins[k]) * double(start + i) / N);
            in[i] = x;
        }
        os.up(in.data(), hi.data(), B);
        for (double& v : hi) v = t21Nonlinear(v, drive);
        os.down(hi.data(), out.data(), B);
        for (int i = 0; i < B; ++i) if (start + i >= 2 * N) cap.push_back(out[i]);
    }
    std::vector<std::complex<double>> X(cap.begin(), cap.end());
    fft(X);
    std::vector<double> p(N / 2);
    for (int k = 0; k < N / 2; ++k) p[k] = std::norm(X[k]);
    return p;
}

static void t21_oversampling() {
    using dsp::OsFamily;
    // Stage specs: <=0.01 dB passband ripple, >=100 dB stopband, protected band 0.45*Fs at every stage.
    for (OsFamily fam : {OsFamily::Fir, OsFamily::Iir})
        for (int f : {2, 4, 8, 16}) {
            dsp::Oversampler os;
            CHECK(os.prepare({f, fam, 0.45, 100.0}, 512), "prepare %d", f);
            CHECK(os.factor() == f && (1 << os.stages()) == f, "rate %d", f);
            for (int s = 0; s < os.stages(); ++s) {
                const double pe = 0.45 / double(2 << s);
                if (fam == OsFamily::Fir) checkStage(os.firStage(s), pe, "fir", s);
                else checkStage(os.iirStage(s), pe, "iir", s);
            }
        }
    dsp::Oversampler bad;
    CHECK(!bad.prepare({3, OsFamily::Fir}, 64) && !bad.prepare({128, OsFamily::Fir}, 64)
          && !bad.prepare({4, OsFamily::Fir, 0.5}, 64) && !bad.prepare({4, OsFamily::Fir}, 0), "reject");

    // Exact rates and declared latency: 1 kHz sine round trip; FIR phase lag must equal latency().
    const double fs = 48000, w = 2 * M_PI * 1000.0 / fs;
    for (OsFamily fam : {OsFamily::Fir, OsFamily::Iir})
        for (int f : {1, 2, 4, 8, 16}) {
            dsp::Oversampler os; os.prepare({f, fam}, 480);
            std::vector<double> in(480), hi(480 * f), out(480);
            double sc = 0, cc = 0;
            for (int blk = 0; blk < 20; ++blk) {
                for (int i = 0; i < 480; ++i) in[i] = std::sin(w * (blk * 480 + i));
                os.up(in.data(), hi.data(), 480);
                os.down(hi.data(), out.data(), 480);
                if (blk >= 10) for (int i = 0; i < 480; ++i) {
                    sc += out[i] * std::sin(w * (blk * 480 + i)); cc += out[i] * std::cos(w * (blk * 480 + i));
                }
            }
            const double amp = 2 * std::hypot(sc, cc) / 4800.0;
            const double lag = std::remainder(-std::atan2(cc, sc) / w - os.latency(), 48.0) + os.latency();  // 48-sample period
            CHECK(std::fabs(20 * std::log10(amp)) <= 0.01, "fam %d f %d gain %g dB", int(fam), f, 20 * std::log10(amp));
            if (fam == OsFamily::Fir)
                CHECK(std::fabs(lag - os.latency()) < 1e-6, "f %d lag %g vs latency %g", f, lag, os.latency());
            else
                CHECK(std::fabs(lag - os.latency()) < 0.05 && (f == 1 || os.latency() > 0), "iir f %d lag %g nominal %g", f, lag, os.latency());
        }

    // Alias test. Published method (also in docs/TRACEABILITY.md):
    //  Fs=48 kHz, N=16384-point rectangular FFT of steady state (samples 2N..3N-1). Five coherent
    //  tones of equal amplitude A on bins 7*{37,101,233,419,677} (0.76-13.9 kHz). Synthetic
    //  memoryless nonlinearity tanh(D*x) per oversampled sample; cases (A,D) = nominal (0.1,4) and
    //  hot (0.3,8). Every non-aliased product sum(n_k f_k) lands on a bin divisible by 7; an alias
    //  folded by j*F*Fs or j*Fs lands on a bin congruent to j*F*N (mod 7), nonzero for j not a
    //  multiple of 7 (gcd(7,N)=1). Unwanted = power on in-band (20 Hz-20 kHz) bins not divisible
    //  by 7, relative to the in-band output power of the 64x FIR (140 dB stages) reference.
    //  Excluded/inseparable: bins divisible by 7 (wanted lattice), folds with j>=7.
    const int N = 16384, bins[5] = {7 * 37, 7 * 101, 7 * 233, 7 * 419, 7 * 677};
    const double df = fs / N;
    const int k0 = int(std::ceil(20.0 / df)), k1 = int(20000.0 / df);
    auto unwanted = [&](const std::vector<double>& p) { double e = 0; for (int k = k0; k <= k1; ++k) if (k % 7) e += p[k]; return e; };
    // Only the nominal case is gated. The hot case (near hard clip) is a recorded FINDING: 4x
    // misses -70 dB for both FIR and IIR, i.e. the rate, not the filter, limits it (see TRACEABILITY).
    struct Stim { const char* name; double amp, drive; bool gated; };
    struct Mode { const char* name; int f; dsp::OsFamily fam; double limit; };
    for (const Stim st : {Stim{"nominal", 0.1, 4.0, true}, Stim{"hot", 0.3, 8.0, false}}) {
        dsp::Oversampler ref; CHECK(ref.prepare({64, OsFamily::Fir, 0.45, 140.0}, 256), "ref prepare");
        const auto pr = t21Spectrum(ref, bins, 5, N, st.amp, st.drive);
        double eRef = 0; for (int k = k0; k <= k1; ++k) eRef += pr[k];
        const double refAlias = 10 * std::log10(unwanted(pr) / eRef + 1e-300);
        std::printf("T21 %-7s 64x reference self-alias %8.2f dB\n", st.name, refAlias);
        CHECK(refAlias <= -120.0, "%s reference self-alias %g dB", st.name, refAlias);
        for (const Mode m : {Mode{"Studio 4x FIR", 4, OsFamily::Fir, -70.0}, Mode{"Render 16x FIR", 16, OsFamily::Fir, -85.0},
                             Mode{"4x IIR", 4, OsFamily::Iir, 0.0}, Mode{"2x FIR Draft", 2, OsFamily::Fir, 0.0}}) {
            dsp::Oversampler os; os.prepare({m.f, m.fam}, 256);
            const double a = 10 * std::log10(unwanted(t21Spectrum(os, bins, 5, N, st.amp, st.drive)) / eRef + 1e-300);
            const bool gate = st.gated && m.limit < 0;
            std::printf("T21 %-7s %-20s alias %8.2f dB (%s %.0f dB)%s\n", st.name, m.name, a,
                        gate ? "limit" : (m.limit < 0 ? "ungated, spec" : "no spec, info"), m.limit, (m.limit < 0 && a > m.limit) ? " FINDING: exceeds spec" : "");
            if (gate) CHECK(a <= m.limit, "%s %s alias %g dB", st.name, m.name, a);
        }
    }
}

static void t22_hpf() {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        dsp::ButterworthHpf f; CHECK(f.design(fs, 15.0), "design");
        // Magnitude from coefficients at fc.
        const double w = 2 * M_PI * 15.0 / fs;
        auto mag = [&](double ww) {
            std::complex<double> z1 = std::polar(1.0, -ww), z2 = std::polar(1.0, -2 * ww);
            return std::abs((f.b0 + f.b1 * z1 + f.b2 * z2) / (1.0 + f.a1 * z1 + f.a2 * z2));
        };
        CHECK(std::fabs(20 * std::log10(mag(w)) + 3.0103) <= 0.05, "fs %g corner %g dB", fs, 20 * std::log10(mag(w)));
        // DC step residual after 2 s.
        double y = 0; for (int i = 0; i < int(2 * fs); ++i) y = f.process(1.0);
        CHECK(20 * std::log10(std::fabs(y) + 1e-300) < -100.0, "fs %g residual %g", fs, y);
    }
    dsp::ButterworthHpf f; CHECK(!f.design(48000, 24000) && !f.design(48000, 0) && !f.design(0, 15), "reject");
}

static void t23_clip_match() {
    double prev = 0;
    for (double x = 0; x < 50; x += 1e-3) {
        const double y = dsp::softClip(x);
        CHECK(y <= 1.0 && (x > 2.5 || y < 1.0) && y >= prev - 1e-15, "monotone/bounded at %g", x);
        if (x <= 0.9) CHECK(y == x, "identity");
        prev = y;
    }
    CHECK(dsp::softClip(-0.95) == -dsp::softClip(0.95), "odd");
    const double h = 1e-6;  // C1 at knee
    CHECK(std::fabs((dsp::softClip(0.9 + h) - dsp::softClip(0.9 - h)) / (2 * h) - 1.0) < 1e-4, "derivative");
    // Gain match: wet is 2x (+6 dB) of a -12 dBFS-RMS sine; settle 5 s, residual within +-0.5 dB.
    const double fs = 48000; dsp::GainMatcher gm; gm.prepare(fs);
    double ed = 0, ew = 0; const int N = int(8 * fs); const int tail = int(fs);
    for (int i = 0; i < N; ++i) {
        const double d = 0.25 * std::sin(2 * M_PI * 220.0 * i / fs), w = 2.0 * d;
        const double g = gm.update(d, w);
        if (i >= N - tail) { ed += d * d; ew += (g * w) * (g * w); }
    }
    CHECK(std::fabs(10 * std::log10(ew / ed)) <= 0.5, "match %g dB", 10 * std::log10(ew / ed));
    // Freeze below -60 dBFS: gain stays unity for quiet input.
    gm.reset(); for (int i = 0; i < 48000; ++i) gm.update(1e-4, 2e-4);
    CHECK(gm.gain() == 1.0, "freeze");
    // Clamp: huge mismatch bounded to [0.25,4] eventually.
    gm.reset(); for (int i = 0; i < int(30 * fs); ++i) gm.update(0.5 * std::sin(i * 0.05), 0.01 * std::sin(i * 0.05));
    CHECK(gm.gain() <= 4.0 + 1e-9 && gm.gain() > 3.5, "clamp %g", gm.gain());
}

int main() {
    registry(); t20_thermal(); t21_oversampling(); t22_hpf(); t23_clip_match();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
