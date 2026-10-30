// Acceptance tests: T20 (thermal), T21 (oversampling), T22 (HPF), T23 (soft clip / gain match),
// registry invariants (P01), strict JSON + device manifests + MNA/Newton on SYNTHETIC fixtures (P02),
// state documents/migration/checkpoints (P04, part of T24).
// Plain-assert harness: no third-party dependency, deterministic; only file I/O is the P04 atomic-save
// check in the system temp directory.
#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include "core/Json.h"
#include "core/ParameterRegistry.h"
#include "dsp/DeviceManifest.h"
#include "dsp/MnaSolver.h"
#include "dsp/OutputProtection.h"
#include "dsp/Oversampling.h"
#include "dsp/Halfband2x.h"
#include "dsp/ThermalNetwork.h"
#include "state/StateDocument.h"
#include <filesystem>
#include <fstream>

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

// ---- P02 strict JSON, device manifests, MNA/Newton (synthetic fixtures only) -----------------
static void p02_json() {
    core::JsonValue v; std::string e;
    CHECK(core::parseJson(R"({"a":[1,-2.5e-3,true,null,"xé😀"],"b":{}})", v, e), "valid: %s", e.c_str());
    core::JsonValue w;
    CHECK(core::parseJson(core::writeJson(v), w, e) && core::writeJson(w) == core::writeJson(v), "roundtrip");
    CHECK(v.get("a")->array[4].string == "x\xC3\xA9\xF0\x9F\x98\x80", "utf8 decode");
    auto n = core::JsonValue::makeNumber(0.1 + 0.2);
    CHECK(core::parseJson(core::writeJson(n), w, e) && w.number == n.number, "exact double");
    const char* badDocs[] = {R"({"a":1,"a":2})", "[1,]", "01", "NaN", "1e999", "-", "1.", R"("\x")", "\"\xC0\xAF\"",
                             "\"\x01\"", "[1] x", R"("\ud800")", R"("\udc00")", "{\"a\" 1}", "tru", "", "\"\xED\xA0\x80\""};
    for (const char* d : badDocs) CHECK(!core::parseJson(d, v, e), "accepted bad doc %s", d);
    std::string deep(21, '['); deep += std::string(21, ']');
    CHECK(!core::parseJson(deep, v, e), "depth 21");
    std::string ok20(20, '['); ok20 += std::string(20, ']');
    CHECK(core::parseJson(ok20, v, e) && core::jsonDepth(v) == 20, "depth 20 %s", e.c_str());
    core::JsonLimits small; small.maxBytes = 4;
    CHECK(!core::parseJson("[1,2]", v, e, small), "size bound");
}

static void p02_manifest() {
    const auto& cat = dsp::syntheticFixtureCatalogue();
    CHECK(cat.size() == 3, "catalogue %zu", cat.size());
    for (const auto& m : cat) CHECK(m.provenance == dsp::ProvenanceKind::SyntheticFixture, "%s synthetic label", m.modelId.c_str());
    const auto* d = dsp::findSyntheticFixture("fixture.diode");
    CHECK(d && d->equation == dsp::DeviceEquation::ShockleyDiode && d->maxExpArg == 40 && d->params.size() == 3, "diode fixture");
    // Mutations of the diode fixture that must be rejected.
    const std::pair<const char*, const char*> muts[] = {
        {"\"kind\":\"synthetic-fixture\"", "\"kind\":\"guessed\""},
        {"\"kind\":\"synthetic-fixture\"", "\"kind\":\"measured\""},           // measured without source
        {"\"equation\":\"shockley-diode\"", "\"equation\":\"12AX7\""},
        {"\"min\":0.5,\"max\":4,\"default\":1}", "\"min\":0.5,\"max\":4,\"default\":5}"},
        {"\"min\":0.5,\"max\":4", "\"min\":5,\"max\":4"},
        {"\"currentA\":1e-3", "\"currentA\":0"},
        {"\"pins\":[\"anode\",\"cathode\"]", "\"pins\":[\"anode\"]"},
        {"\"unit\":\"A\"", "\"unit\":\"mA\""},
        {"\"maxExpArg\":40", "\"maxExpArg\":-1"},
        {",\"domain\":{\"maxExpArg\":40}", ""},
        {"\"revision\":1", "\"revision\":1,\"extra\":0"},
        {"\"schemaVersion\":1", "\"schemaVersion\":2"},
    };
    for (const auto& mu : muts) {
        std::string t = dsp::syntheticFixtureJson(2);
        const auto at = t.find(mu.first);
        CHECK(at != std::string::npos, "mutation anchor %s", mu.first);
        if (at == std::string::npos) continue;
        t.replace(at, std::strlen(mu.first), mu.second);
        core::JsonValue j; std::string e; dsp::DeviceManifest m;
        CHECK(!(core::parseJson(t, j, e) && dsp::parseDeviceManifest(j, m, e)), "accepted mutation -> %s", mu.second);
    }
}

static dsp::NetElement dev(const char* id, int a, int b, std::initializer_list<double> p) {
    dsp::NetElement e; e.model = dsp::findSyntheticFixture(id); e.a = a; e.b = b;
    int k = 0; for (double v : p) e.param[k++] = v;
    return e;
}
static dsp::NetElement src(dsp::NetElement::Kind k, int a, int b, double v) { dsp::NetElement e; e.kind = k; e.a = a; e.b = b; e.value = v; return e; }
using K = dsp::NetElement::Kind;
const double kIs = 1e-14, kN = 1.0, kVt = 0.025852;  // fixture.diode defaults (synthetic)
static double diodeI(double v) { const double a = v / (kN * kVt); return a > 40 ? kIs * (std::exp(40.0) * (1 + a - 40) - 1) : kIs * (std::exp(a) - 1); }

// Root of a strictly increasing scalar function by bisection (independent of Newton/LU).
template <class Fn> static double bisect(Fn g, double lo, double hi) {
    for (int i = 0; i < 200; ++i) { const double mid = 0.5 * (lo + hi); (g(mid) > 0 ? hi : lo) = mid; }
    return 0.5 * (lo + hi);
}

static void p02_mna() {
    std::string e;
    // (1) Analytic Jacobian vs central differences, diode biased across and beyond the exp cap.
    {
        dsp::Netlist n; n.nodes = 2;
        n.elements = {src(K::VoltageSource, 1, 0, 0.0), dev("fixture.resistor", 1, 2, {100.0}),
                      dev("fixture.diode", 2, 0, {kIs, kN, kVt}), dev("fixture.capacitor", 2, 0, {1e-8})};
        dsp::MnaSolver s; CHECK(s.prepare(n, 1e-5, {}, e), "prepare jac: %s", e.c_str());
        const int m = s.unknowns();
        for (double vd : {-1.0, 0.0, 0.3, 0.6, 0.9, 1.2}) {
            std::vector<double> x = {0.5, vd, 1e-3}, F(m), J(m * m), Fp(m), Fm(m);
            s.evaluate(x.data(), F.data(), J.data(), false);
            for (int c = 0; c < m; ++c) {
                const double hstep = 1e-7 * (1 + std::fabs(x[c]));
                auto xp = x, xm = x; xp[c] += hstep; xm[c] -= hstep;
                s.evaluate(xp.data(), Fp.data(), nullptr, false); s.evaluate(xm.data(), Fm.data(), nullptr, false);
                for (int r = 0; r < m; ++r) {
                    // FD noise scales with |F_r|/hstep, so tolerance is relative to the row's largest entry.
                    double rowMax = 0; for (int q = 0; q < m; ++q) rowMax = std::max(rowMax, std::fabs(J[r * m + q]));
                    const double fd = (Fp[r] - Fm[r]) / (2 * hstep), an = J[r * m + c];
                    CHECK(std::fabs(fd - an) <= 1e-6 * (rowMax + 1e-3), "J[%d][%d] vd %g an %g fd %g", r, c, vd, an, fd);
                }
            }
        }
    }
    // (2) Synthetic RC (R=10k, C=10n, tau=100 us) step at 192 kHz vs the exact BE recurrence.
    {
        dsp::Netlist n; n.nodes = 2;
        n.elements = {src(K::VoltageSource, 1, 0, 0.0), dev("fixture.resistor", 1, 2, {1e4}), dev("fixture.capacitor", 2, 0, {1e-8})};
        const double h = 1.0 / 192000, r = h / 1e-4;
        dsp::MnaSolver s; CHECK(s.prepare(n, h, {8, 1e-7}, e), "prepare rc: %s", e.c_str());
        s.setSource(0, 1.0);
        double ref = 0, worst = 0, worstA = 0;
        for (int i = 1; i <= 1920; ++i) {
            CHECK(s.step(), "rc step %d", i);
            ref = (ref + r) / (1 + r);
            worst = std::max(worst, std::fabs(s.voltage(2) - ref));
            worstA = std::max(worstA, std::fabs(s.voltage(2) - (1 - std::exp(-i * h / 1e-4))));
            CHECK(s.lastIterations() <= 2, "linear iterations %d", s.lastIterations());
        }
        CHECK(worst <= 1e-12, "rc vs BE recurrence %g", worst);
        CHECK(worstA <= 0.5 * r, "rc vs analytic %g (BE first-order bound %g)", worstA, 0.5 * r);
        CHECK(std::fabs(s.unknown(2) + (1 - s.voltage(2)) / 1e4) < 1e-12, "source branch current");
    }
    // (3) Synthetic diode + resistor DC point vs bisection on (V-v)/R = I(v).
    {
        dsp::Netlist n; n.nodes = 2;
        n.elements = {src(K::VoltageSource, 1, 0, 1.0), dev("fixture.resistor", 1, 2, {1000.0}), dev("fixture.diode", 2, 0, {kIs, kN, kVt})};
        dsp::MnaSolver s; CHECK(s.prepare(n, 1e-5, {}, e), "prepare dc: %s", e.c_str());
        const double vd = bisect([](double v) { return diodeI(v) - (1.0 - v) / 1000.0; }, 0.0, 1.0);
        CHECK(std::fabs(s.voltage(2) - vd) <= 1e-9, "diode dc %.12f ref %.12f", s.voltage(2), vd);
        CHECK(s.lastScaledResidual() <= 1e-7 && s.lastIterations() <= dsp::MnaSolver::kDcIterations, "dc residual %g iters %d",
              s.lastScaledResidual(), s.lastIterations());
    }
    // (4) Synthetic antiparallel-diode clipper with C, 2 V 1 kHz drive, 192 kHz, Studio cap 8:
    //     per-step reference solves the same BE scalar equation by bisection.
    {
        dsp::Netlist n; n.nodes = 2;
        n.elements = {src(K::VoltageSource, 1, 0, 0.0), dev("fixture.resistor", 1, 2, {1000.0}), dev("fixture.diode", 2, 0, {kIs, kN, kVt}),
                      dev("fixture.diode", 0, 2, {kIs, kN, kVt}), dev("fixture.capacitor", 2, 0, {1e-8})};
        const double h = 1.0 / 192000, C = 1e-8;
        dsp::MnaSolver s; CHECK(s.prepare(n, h, {8, 1e-7}, e), "prepare clip: %s", e.c_str());
        double vref = 0, worst = 0; int maxIt = 0;
        for (int i = 1; i <= 384; ++i) {
            const double vs = 2.0 * std::sin(2 * M_PI * 1000.0 * i * h);
            s.setSource(0, vs);
            CHECK(s.step(), "clip step %d", i);
            const double vo = vref;
            vref = bisect([&](double v) { return C * (v - vo) / h + (v - vs) / 1000.0 + diodeI(v) - diodeI(-v); }, -3.0, 3.0);
            worst = std::max(worst, std::fabs(s.voltage(2) - vref));
            maxIt = std::max(maxIt, s.lastIterations());
        }
        CHECK(worst <= 1e-8, "clipper vs bisection %g V", worst);
        CHECK(maxIt <= 8 && s.failures() == 0, "clipper iterations %d failures %ld", maxIt, s.failures());
        // Failure containment: 1 Newton iteration cannot converge a large nonlinear jump.
        dsp::MnaSolver f; CHECK(f.prepare(n, h, {1, 1e-7}, e), "prepare cap1");
        std::vector<double> before(f.stateSize()), after(f.stateSize());
        for (int k = 0; k < 3; ++k) {
            f.getState(before.data());
            f.setSource(0, k % 2 ? -50.0 : 50.0);
            CHECK(!f.step(), "cap1 step %d should fail", k);
            f.getState(after.data());
            CHECK(before == after, "failed step committed state");
        }
        CHECK(f.failures() == 3 && f.needsFallback(), "fallback flag");
    }
    // (5) Rejections.
    {
        dsp::MnaSolver s;
        dsp::Netlist n; n.nodes = 2;
        n.elements = {src(K::VoltageSource, 1, 0, 1.0), dev("fixture.capacitor", 1, 2, {1e-8})};  // node 2 floating at DC
        CHECK(!s.prepare(n, 1e-5, {}, e), "floating accepted");
        n.elements = {src(K::VoltageSource, 1, 0, 1.0), src(K::VoltageSource, 1, 0, 2.0), dev("fixture.resistor", 1, 2, {1e3}),
                      dev("fixture.resistor", 2, 0, {1e3})};
        CHECK(!s.prepare(n, 1e-5, {}, e), "V loop accepted");
        n.elements = {src(K::VoltageSource, 1, 0, 1.0), dev("fixture.resistor", 1, 2, {0.0}), dev("fixture.resistor", 2, 0, {1e3})};
        CHECK(!s.prepare(n, 1e-5, {}, e), "R outside manifest bounds accepted");
        n.elements = {src(K::VoltageSource, 1, 0, 1.0), dev("fixture.resistor", 1, 1, {1e3}), dev("fixture.resistor", 2, 0, {1e3})};
        CHECK(!s.prepare(n, 1e-5, {}, e), "a==b accepted");
        n.elements = {src(K::VoltageSource, 1, 0, 1.0), dev("fixture.resistor", 1, 2, {1e3}), dev("fixture.resistor", 2, 0, {1e3})};
        CHECK(s.prepare(n, 1e-5, {}, e), "divider: %s", e.c_str());
        CHECK(std::fabs(s.voltage(2) - 0.5) < 1e-12, "divider %g", s.voltage(2));
        CHECK(!s.prepare(n, 0.0, {}, e) && !s.prepare(n, 1e-5, {13, 1e-7}, e), "h/options accepted");
        dsp::Netlist sc = n; sc.currentScaleA = 0; CHECK(!s.prepare(sc, 1e-5, {}, e), "scale accepted");
        dsp::Netlist big = n; big.nodes = 33; CHECK(!s.prepare(big, 1e-5, {}, e), "33 nodes accepted");
        dsp::Netlist nm = n; nm.elements[1].model = nullptr; CHECK(!s.prepare(nm, 1e-5, {}, e), "missing manifest accepted");
        dsp::Netlist nl; nl.nodes = 1; nl.elements.push_back(src(K::VoltageSource, 1, 0, 1.0));
        for (int k = 0; k < 9; ++k) nl.elements.push_back(dev("fixture.diode", 1, 0, {kIs, kN, kVt}));
        CHECK(!s.prepare(nl, 1e-5, {}, e), "9 nonlinear accepted");
    }
}

// ---- P04 state documents ---------------------------------------------------------------------
// SYNTHETIC fixture documents: values are arbitrary, not authored factory content.
static const char* kSession = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"session","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic session","tags":["test"],"createdAt":"2026-10-30T12:00:00Z",
 "requiredFeatures":["CD-F21"],"blueprints":[{"schemaVersion":1,"id":"example.rc","components":[{"id":"R1","modelId":"fixture.resistor"}]}],
 "baseParameters":{"hpf.frequency":20,"quality.oversampling":3},"macros":{},"timeline":{},"faults":[],"seed":42,
 "ui":{"theme":"dark"},"x-note":"kept"})";

static std::uint64_t fnv1a(const std::string& t) { std::uint64_t h = 1469598103934665603ull; for (unsigned char c : t) { h ^= c; h *= 1099511628211ull; } return h; }

// Test-only synthetic v0 -> v1 migration: v0 called the parameter map "params".
static bool migrateFixtureV0(core::JsonValue& d, std::string& why) {
    core::JsonValue* p = d.get("params");
    if (!p) { why = "v0 without params"; return false; }
    core::JsonValue moved = *p;
    for (std::size_t k = 0; k < d.object.size(); ++k) if (d.object[k].first == "params") { d.object.erase(d.object.begin() + long(k)); break; }
    d.set("baseParameters", std::move(moved));
    d.set("schemaVersion", core::JsonValue::makeNumber(1));
    return true;
}
static bool migrateBroken(core::JsonValue&, std::string&) { return true; }  // forgets to advance version

static void p04_state() {
    state::StateDocument d; std::string e; state::LoadReport rep;
    CHECK(state::loadState(kSession, d, e, state::productionMigrations(), &rep), "load: %s", e.c_str());
    CHECK(d.kind == state::DocKind::Session && d.seed == 42 && d.baseParameters.size() == 2 && d.ui && d.metadata.size() == 1, "fields");
    CHECK(rep.sourceVersion == 1 && rep.migrationsApplied == 0, "report");
    // Save -> load -> save is a fixed point (canonical).
    const std::string s1 = state::saveState(d);
    state::StateDocument d2; CHECK(state::loadState(s1, d2, e), "reload: %s", e.c_str());
    CHECK(state::saveState(d2) == s1, "canonical roundtrip");

    // Rejections (each a single mutation of the synthetic session).
    const std::pair<const char*, const char*> muts[] = {
        {"\"seed\":42", "\"seed\":42,\"seed\":43"},                   // duplicate key
        {"\"seed\":42", "\"seed\":4.5"},
        {"\"seed\":42", "\"seed\":1e999"},                             // non-finite
        {"\"schemaVersion\":1,\"product\"", "\"schemaVersion\":2,\"product\""},  // newer major
        {"\"minReaderVersion\":1", "\"minReaderVersion\":2"},
        {"\"product\":\"CircuitDrift\"", "\"product\":\"Other\""},
        {"\"kind\":\"session\"", "\"kind\":\"preset\""},
        {"\"CD-F21\"", "\"CD-F99\""},                                  // unknown required feature
        {"\"fixture.resistor\"", "\"12AX7\""},                         // unknown model (G01)
        {"\"hpf.frequency\":20", "\"hpf.frequency\":5"},               // out of range: rejected, not clamped
        {"\"hpf.frequency\":20", "\"hpf.nope\":20"},                   // unknown parameter
        {"\"quality.oversampling\":3", "\"quality.oversampling\":2.5"},
        {"\"2026-10-30T12:00:00Z\"", "\"yesterday\""},
        {"\"faults\":[]", "\"faults\":1"},
        {"\"kind\":\"session\"", "\"kind\":\"scene\""},                // scene with 'blueprints'
    };
    for (const auto& mu : muts) {
        std::string t = kSession;
        const auto at = t.find(mu.first);
        CHECK(at != std::string::npos, "anchor %s", mu.first);
        if (at == std::string::npos) continue;
        t.replace(at, std::strlen(mu.first), mu.second);
        state::StateDocument x;
        CHECK(!state::loadState(t, x, e), "accepted mutation %s", mu.second);
    }
    std::string deep = std::string(kSession);
    deep.replace(deep.find("\"macros\":{}"), 11, "\"macros\":" + std::string(19, '[') + std::string(19, ']'));
    CHECK(!state::loadState(deep, d2, e), "depth > 20 accepted");
    std::string huge = std::string(kSession);
    huge.replace(huge.find("\"x-note\":\"kept\""), 15, "\"x-note\":\"" + std::string(70000, 'a') + "\"");
    CHECK(!state::loadState(huge, d2, e), "metadata bound");
    std::string bigScene = std::string(kSession);
    bigScene.replace(bigScene.find("\"kind\":\"session\""), 16, "\"kind\":\"scene\"");
    bigScene.replace(bigScene.find("\"x-note\":\"kept\""), 15, "\"x-pad\":\"" + std::string(1400000, ' ') + "\"");
    CHECK(!state::loadState(bigScene, d2, e) && e.find("1.25") != std::string::npos, "scene size bound: %s", e.c_str());

    // Publish: scene only; strips ui/checkpoint/metadata; result reloads.
    std::string sceneTxt = kSession;
    sceneTxt.replace(sceneTxt.find("\"kind\":\"session\""), 16, "\"kind\":\"scene\"");
    sceneTxt.replace(sceneTxt.find("\"blueprints\":["), 14, "\"blueprint\":");
    sceneTxt.replace(sceneTxt.find("}]}],"), 5, "}]},");
    state::StateDocument sc; CHECK(state::loadState(sceneTxt, sc, e), "scene load: %s", e.c_str());
    std::string pub; CHECK(state::publishScene(sc, pub, e), "publish: %s", e.c_str());
    CHECK(pub.find("\"ui\"") == std::string::npos && pub.find("x-note") == std::string::npos, "publish strips private fields");
    CHECK(!state::publishScene(d, pub, e), "session published");
    state::StateDocument withCp = sc;
    withCp.checkpoint = state::EngineCheckpoint{};
    CHECK(state::publishScene(withCp, pub, e) && pub.find("checkpoint") == std::string::npos, "publish strips checkpoint");

    // Migrations (synthetic test-only v0 chain; production chain is empty because v1 is first).
    std::string v0 = kSession;
    v0.replace(v0.find("\"schemaVersion\":1,\"product\""), 27, "\"schemaVersion\":0,\"product\"");
    v0.replace(v0.find("\"baseParameters\""), 16, "\"params\"");
    CHECK(!state::loadState(v0, d2, e), "v0 accepted without chain");
    state::MigrationChain chain{0, {&migrateFixtureV0}};
    CHECK(state::loadState(v0, d2, e, chain, &rep) && rep.sourceVersion == 0 && rep.migrationsApplied == 1, "migrate: %s", e.c_str());
    CHECK(state::saveState(d2) == s1, "migrated result equals v1 document");
    const std::uint64_t digest = fnv1a(state::saveState(d2));
    CHECK(digest == 0x9b1c4df78b58de31ull, "migration digest %016llx", (unsigned long long)digest);  // frozen fixture digest
    state::MigrationChain broken{0, {&migrateBroken}};
    CHECK(!state::loadState(v0, d2, e, broken), "non-advancing migration accepted");
    state::MigrationChain gap{0, {}};
    CHECK(!state::loadState(v0, d2, e, gap), "missing step accepted");

    // Checkpoint: exact continuation of the synthetic diode clipper through save/load (T24 part).
    dsp::Netlist n; n.nodes = 2;
    n.elements = {src(K::VoltageSource, 1, 0, 0.0), dev("fixture.resistor", 1, 2, {1000.0}), dev("fixture.diode", 2, 0, {kIs, kN, kVt}),
                  dev("fixture.diode", 0, 2, {kIs, kN, kVt}), dev("fixture.capacitor", 2, 0, {1e-8})};
    const double h = 1.0 / 192000;
    auto drive = [&](dsp::MnaSolver& s, int i) { s.setSource(0, 2.0 * std::sin(2 * M_PI * 1000.0 * i * h)); s.step(); };
    dsp::MnaSolver a; CHECK(a.prepare(n, h, {}, e), "cp prepare");
    for (int i = 1; i <= 100; ++i) drive(a, i);
    state::EngineCheckpoint cp{"0.0.1-dev", "fixture-v1", 48000, 4, 100, std::vector<double>(a.stateSize()), {}, {0x0123456789abcdefull, ~0ull}};
    a.getState(cp.electrical.data());
    state::StateDocument sess = d; sess.checkpoint = cp;
    state::StateDocument back; CHECK(state::loadState(state::saveState(sess), back, e), "cp load: %s", e.c_str());
    CHECK(back.checkpoint && back.checkpoint->electrical == cp.electrical && back.checkpoint->rng == cp.rng
          && back.checkpoint->sampleIndex == 100, "checkpoint exact roundtrip");
    state::RuntimeIdentity rt{"0.0.1-dev", "fixture-v1", 48000, 4, std::size_t(a.stateSize()), 0};
    std::string why;
    CHECK(state::decideRestore(back, rt, why) == state::RestoreDecision::Restore, "restore: %s", why.c_str());
    dsp::MnaSolver b; b.prepare(n, h, {}, e); b.setState(back.checkpoint->electrical.data());
    double worst = 0;
    for (int i = 101; i <= 300; ++i) { drive(a, i); drive(b, i); worst = std::max(worst, std::fabs(a.voltage(2) - b.voltage(2))); }
    CHECK(worst <= 1e-6, "continuation %g", worst);
    std::printf("P04 checkpoint continuation peak error %.3g V (T24 limit 1e-6)\n", worst);
    for (auto mutate : {+[](state::RuntimeIdentity& r) { r.sampleRate = 44100; }, +[](state::RuntimeIdentity& r) { r.oversampling = 16; },
                        +[](state::RuntimeIdentity& r) { r.modelRevision = "fixture-v2"; }, +[](state::RuntimeIdentity& r) { r.electricalSize += 1; }}) {
        state::RuntimeIdentity r2 = rt; mutate(r2);
        CHECK(state::decideRestore(back, r2, why) == state::RestoreDecision::ResetWarm && !why.empty(), "incompatible restore");
    }
    CHECK(state::decideRestore(d, rt, why) == state::RestoreDecision::NoCheckpoint, "no checkpoint");
    std::string cpBad = state::saveState(sess);
    cpBad.replace(cpBad.find("0123456789abcdef"), 16, "0123456789ABCDEF");
    CHECK(!state::loadState(cpBad, back, e), "bad rng hex accepted");

    // Atomic save.
    const auto path = (std::filesystem::temp_directory_path() / "cd_p04_atomic.circuitdrift.json").string();
    CHECK(state::saveStateFileAtomic(path, s1, e), "atomic save: %s", e.c_str());
    std::ifstream f(path, std::ios::binary); std::string got((std::istreambuf_iterator<char>(f)), {});
    CHECK(got == s1 && !std::filesystem::exists(path + ".tmp"), "atomic content");
    std::filesystem::remove(path);
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

// T21 (partial): 2x halfband stage. Passband ripple <=0.01 dB to 0.2 fs_hi, stopband <=-100 dB from 0.3 fs_hi.
static void t21_halfband() {
    dsp::Halfband2x hb; hb.prepare();
    const auto& h = hb.coefficients();
    for (std::size_t n = 0; n < dsp::Halfband2x::kTaps; ++n) {
        CHECK(std::fabs(h[n] - h[dsp::Halfband2x::kTaps - 1 - n]) < 1e-15, "symmetry %zu", n);
        const long off = long(n) - long(dsp::Halfband2x::kHalf);
        if (off != 0 && off % 2 == 0) CHECK(std::fabs(h[n]) < 1e-15, "half-band zero at %zu", n);
    }
    auto mag = [&](double f) {
        double re = 0, im = 0;
        for (std::size_t n = 0; n < dsp::Halfband2x::kTaps; ++n) {
            re += h[n] * std::cos(2 * M_PI * f * n); im -= h[n] * std::sin(2 * M_PI * f * n);
        }
        return std::hypot(re, im);
    };
    double worstPass = 0, worstStop = -1e9;
    for (int i = 0; i <= 400; ++i) {
        worstPass = std::max(worstPass, std::fabs(20 * std::log10(mag(0.2 * i / 400.0))));
        worstStop = std::max(worstStop, 20 * std::log10(mag(0.3 + 0.2 * i / 400.0)));
    }
    CHECK(worstPass <= 0.01, "passband ripple %g dB", worstPass);
    CHECK(worstStop <= -100.0, "stopband %g dB", worstStop);
    // Round trip of an in-band tone at fs_lo=48 kHz: quadrature amplitude over steady state within 0.01 dB.
    dsp::Halfband2x up, down; up.prepare(); down.prepare();
    const double fs = 48000; const int total = int(fs), from = total / 2;
    double inI = 0, inQ = 0, outI = 0, outQ = 0;
    for (int i = 0; i < total; ++i) {
        const double x = std::sin(2 * M_PI * 1000.0 * i / fs);
        double a, b; up.upsample(x, a, b);
        const double y = down.downsample(a, b);
        if (i >= from) {
            const double c = std::cos(2 * M_PI * 1000.0 * i / fs), sn = std::sin(2 * M_PI * 1000.0 * i / fs);
            inI += x * c; inQ += x * sn; outI += y * c; outQ += y * sn;
        }
    }
    const double peakIn = std::hypot(inI, inQ), peakOut = std::hypot(outI, outQ);
    CHECK(std::fabs(20 * std::log10(peakOut / peakIn)) <= 0.01, "round trip %g dB", 20 * std::log10(peakOut / peakIn));
}

int main() {
    registry(); t20_thermal(); t21_oversampling(); t21_halfband(); t22_hpf(); t23_clip_match(); p02_json(); p02_manifest(); p02_mna(); p04_state();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
