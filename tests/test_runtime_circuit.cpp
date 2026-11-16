// CircuitDrift tests: P03 integration of MnaSolver inside Oversampler (CircuitRuntime + Engine).
// All circuits are labelled SYNTHETIC fixtures (catalogue fixture.* manifests); nothing here is a
// physical validation (G01 open). Owner: runtime agent.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "core/Json.h"
#include "dsp/DeviceManifest.h"
#include "runtime/CircuitRuntime.h"
#include "runtime/Engine.h"

namespace {
using namespace cd;
using namespace cd::runtime;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

struct Lcg {
    uint64_t s;
    double next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return double(s >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0; }
};

// Runs a mono/stereo CircuitRuntime over x (same signal on every channel) in blocks of `block`.
std::vector<double> runCircuit(CircuitRuntime& rt, const std::vector<double>& x, int block, std::vector<double>* dryOut = nullptr) {
    std::vector<double> wet(x.size()), dry(x.size());
    const int C = rt.channels();
    std::vector<std::vector<double>> wb(C, std::vector<double>(block)), db(C, std::vector<double>(block));
    for (std::size_t pos = 0; pos < x.size(); pos += std::size_t(block)) {
        const int n = int(std::min<std::size_t>(std::size_t(block), x.size() - pos));
        const double* in[2] = {x.data() + pos, x.data() + pos};
        double* w[2] = {wb[0].data(), C > 1 ? wb[1].data() : nullptr};
        double* d[2] = {db[0].data(), C > 1 ? db[1].data() : nullptr};
        rt.process(in, w, d, n);
        std::copy(wb[0].begin(), wb[0].begin() + n, wet.begin() + long(pos));
        std::copy(db[0].begin(), db[0].begin() + n, dry.begin() + long(pos));
    }
    if (dryOut) *dryOut = dry;
    return wet;
}

CircuitConfig withRate(CircuitConfig c, double fs, int factor, dsp::OsFamily fam, bool pad, int maxBlock = 512) {
    c.sampleRate = fs; c.os.factor = factor; c.os.family = fam; c.padToFamilyMax = pad; c.maxBlock = maxBlock;
    return c;
}

// Quadrature amplitude/phase of a tone over [from, to): returns gain and lag in samples vs sin(w n).
void toneFit(const std::vector<double>& y, double w, std::size_t from, std::size_t to, double& amp, double& lag, double period) {
    double sc = 0, cc = 0;
    for (std::size_t n = from; n < to; ++n) { sc += y[n] * std::sin(w * double(n)); cc += y[n] * std::cos(w * double(n)); }
    amp = 2 * std::hypot(sc, cc) / double(to - from);
    lag = -std::atan2(cc, sc) / w;
    lag = std::fmod(lag, period); if (lag < 0) lag += period;
}

// Phase lag is only known modulo the tone period; place it on the period nearest the expectation
// (the integer part is verified separately by impulse-peak position / exact dry delay).
double unwrap(double lag, double expected, double period) { return expected + std::remainder(lag - expected, period); }

std::vector<double> sine(std::size_t n, double amp, double w) {
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = amp * std::sin(w * double(i));
    return x;
}

// ---- Latency plan (01-runtime: pad within family to max integer latency among 1-16x) ----------
void p03_latency_plan() {
    for (auto fam : {dsp::OsFamily::Fir, dsp::OsFamily::Iir}) {
        int declared = -1;
        for (int f : {1, 2, 4, 8, 16}) {
            dsp::Oversampler::Spec s; s.factor = f; s.family = fam;
            LatencyPlan p; CHECK(planLatency(s, true, p), "plan %d", f);
            std::printf("P03 latency %s %2dx: os %.6f base, family max %.6f, declared %d, pad %d high-rate, residual %.3g\n",
                        fam == dsp::OsFamily::Fir ? "FIR" : "IIR", f, p.osLatency, p.familyMax, p.declared, p.padHigh, p.residual);
            if (declared < 0) declared = p.declared;
            CHECK(p.declared == declared, "declared latency differs across rates within family (%d vs %d)", p.declared, declared);
            if (fam == dsp::OsFamily::Fir) CHECK(p.residual == 0.0, "FIR residual %g", p.residual);
            else CHECK(std::fabs(p.residual) <= 0.5 / f + 1e-12, "IIR residual %g", p.residual);
        }
    }
    dsp::Oversampler::Spec s; s.factor = 1;
    LatencyPlan z; CHECK(planLatency(s, false, z) && z.declared == 0 && z.padHigh == 0, "zero-latency 1x declared %d", z.declared);
    s.factor = 3; CHECK(!planLatency(s, true, z), "factor 3 plan accepted");
}

// ---- Independent references --------------------------------------------------------------------
void p03_rc_reference() {
    const double R = 1000, Cap = 1e-8, fs = 48000;
    // (a) 1x: BE recurrence v=(v+a u)/(1+a), a=h/RC, computed independently of MNA/LU.
    {
        CircuitRuntime rt; std::string e;
        CHECK(rt.prepare(withRate(syntheticRcLowpass(R, Cap), fs, 1, dsp::OsFamily::Fir, false), e), "rc prepare: %s", e.c_str());
        Lcg g{7}; std::vector<double> x(20000); for (double& v : x) v = 0.8 * g.next();
        const auto y = runCircuit(rt, x, 64);
        const double a = (1.0 / fs) / (R * Cap);
        double v = 0, worst = 0;
        for (std::size_t n = 0; n < x.size(); ++n) { v = (v + a * x[n]) / (1 + a); worst = std::max(worst, std::fabs(y[n] - v)); }
        std::printf("P03 RC 1x vs BE recurrence: max err %.3g V\n", worst);
        CHECK(worst <= 1e-12, "RC 1x error %g", worst);
    }
    // (b) 4x FIR padded: 1 kHz steady-state gain/phase vs the BE transfer function at the high rate
    // plus the declared integer latency.
    {
        CircuitRuntime rt; std::string e;
        CHECK(rt.prepare(withRate(syntheticRcLowpass(R, Cap), fs, 4, dsp::OsFamily::Fir, true), e), "rc4 prepare: %s", e.c_str());
        const double w = 2 * M_PI * 1000 / fs;
        const auto y = runCircuit(rt, sine(9600, 1.0, w), 128);
        double amp, lag; toneFit(y, w, 4800, 9600, amp, lag, 48.0);
        const double a = (1.0 / (4 * fs)) / (R * Cap), wh = w / 4;
        const std::complex<double> H = a / ((1.0 + a) - std::polar(1.0, -wh));
        const double expLag = rt.latencySamples() - std::arg(H) / w;
        lag = unwrap(lag, expLag, 48.0);
        const double gdb = 20 * std::log10(amp / std::abs(H));
        std::printf("P03 RC 4x vs BE(H) + %d samples: gain err %.4f dB, lag err %.3g samples\n", rt.latencySamples(), gdb, lag - expLag);
        CHECK(std::fabs(gdb) <= 0.01, "RC 4x gain error %g dB", gdb);
        CHECK(std::fabs(lag - expLag) <= 1e-5, "RC 4x lag %g expected %g", lag, expLag);
    }
}

// Strictly increasing node equation of the synthetic clipper; root by bisection (no Newton/LU).
double diodeI(double v) {
    const double is = 1e-14, nvt = 0.025852, a = v / nvt;
    return a > 40 ? is * (std::exp(40.0) * (1 + a - 40) - 1) : is * (std::exp(a) - 1);
}

void p03_diode_reference() {
    const double fs = 192000, R = 1000, Cap = 1e-8, h = 1.0 / fs;
    CircuitRuntime rt; std::string e;
    CHECK(rt.prepare(withRate(syntheticDiodeClipper(), fs, 1, dsp::OsFamily::Fir, false), e), "clipper prepare: %s", e.c_str());
    const auto x = sine(9600, 2.0, 2 * M_PI * 1000 / fs);
    const auto y = runCircuit(rt, x, 127);
    double v = 0, worst = 0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double u = x[n], vo = v;
        auto g = [&](double z) { return (z - u) / R + diodeI(z) - diodeI(-z) + Cap * (z - vo) / h; };
        double lo = -3, hi = 3;
        for (int k = 0; k < 200; ++k) { const double m = 0.5 * (lo + hi); (g(m) > 0 ? hi : lo) = m; }
        v = 0.5 * (lo + hi);
        worst = std::max(worst, std::fabs(y[n] - v));
    }
    std::printf("P03 diode clipper 1x (192 kHz, 2 V) vs bisection: max err %.3g V, failed substeps %ld\n", worst,
                rt.diagnostics().failedSubsteps);
    CHECK(worst <= 1e-8 && rt.diagnostics().failedSubsteps == 0, "clipper error %g", worst);
}

// ---- Measured latency: circuit stage and full engine -----------------------------------------
EngineConfig dividerEngine(double fs, QualitySettings q, int channels = 1) {
    EngineConfig c;
    c.sampleRate = fs; c.maxBlock = 512; c.channels = channels; c.quality = q;
    c.circuit = syntheticDivider(2.0);  // unity linear wet
    c.initial.hpfEnabled = false; c.initial.softClipEnabled = false; c.initial.mix = 1.0;
    return c;
}

std::vector<double> runEngine(Engine& eng, const std::vector<std::vector<double>>& x, int block, std::vector<std::vector<double>>& y,
                              const std::vector<std::pair<int64_t, ParamEvent>>* events = nullptr, int zeroEvery = 0) {
    const int C = int(x.size());
    const std::size_t N = x[0].size();
    y.assign(std::size_t(C), std::vector<double>(N));
    std::vector<ParamEvent> evs;
    std::size_t evi = 0;
    int calls = 0;
    for (std::size_t pos = 0; pos < N;) {
        const uint32_t n = uint32_t(std::min<std::size_t>(std::size_t(block), N - pos));
        evs.clear();
        // Zero-frame calls deliver events due exactly at this position, before the block.
        if (zeroEvery > 0 && (calls++ % zeroEvery) == 0) {
            while (events && evi < events->size() && (*events)[evi].first == int64_t(pos)) {
                ParamEvent ev = (*events)[evi++].second; ev.offset = 0; evs.push_back(ev);
            }
            ProcessBlock zb; zb.ctx = {eng.active()->circuit().sampleRate(), int64_t(pos), 0, 0, false};
            zb.buses.channels = C; zb.events = evs.data(); zb.eventCount = uint32_t(evs.size());
            eng.process(zb);
            evs.clear();
        }
        while (events && evi < events->size() && (*events)[evi].first < int64_t(pos + n)) {
            ParamEvent ev = (*events)[evi++].second; ev.offset = uint32_t((*events)[evi - 1].first - int64_t(pos)); evs.push_back(ev);
        }
        const double* in[2] = {x[0].data() + pos, C > 1 ? x[1].data() + pos : nullptr};
        double* out[2] = {y[0].data() + pos, C > 1 ? y[1].data() + pos : nullptr};
        ProcessBlock b; b.ctx = {eng.active()->circuit().sampleRate(), int64_t(pos), 0, n, false};
        b.buses = {C, in, out}; b.events = evs.data(); b.eventCount = uint32_t(evs.size());
        const auto r = eng.process(b);
        if (r.frames != n) { std::printf("process returned %u of %u\n", r.frames, n); ++g_fail; }
        pos += n;
    }
    return y[0];
}

void p03_measured_latency() {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const double w = 2 * M_PI * 1000 / fs;
        const std::size_t N = std::size_t(fs);
        for (auto fam : {dsp::OsFamily::Fir, dsp::OsFamily::Iir})
            for (int f : {1, 2, 4, 16}) {
                if (fam == dsp::OsFamily::Iir && f != 4) continue;
                CircuitRuntime rt; std::string e;
                CHECK(rt.prepare(withRate(syntheticDivider(2.0), fs, f, fam, true), e), "divider prepare %s", e.c_str());
                std::vector<double> dry;
                const auto y = runCircuit(rt, sine(N / 4, 0.5, w), 256, &dry);
                // Window: 100 periods of 1 kHz exactly (period fs/1000 may be fractional at 44.1 kHz).
                double amp, lag;
                toneFit(y, w, N / 8, N / 8 + std::size_t(fs / 10.0), amp, lag, fs / 1000.0);
                // IIR: compare with the nominal DC delay actually realised (declared minus residual).
                const double D = rt.latencySamples() - rt.latencyPlan().residual;
                const double tol = fam == dsp::OsFamily::Fir ? 1e-6 : 0.05;
                lag = unwrap(lag, D, fs / 1000.0);
                std::printf("P03 measured latency fs %6.0f %s %2dx: declared %d, 1 kHz lag %.7f, gain %+.4f dB\n", fs,
                            fam == dsp::OsFamily::Fir ? "FIR" : "IIR", f, int(D), lag, 20 * std::log10(amp / 0.5));
                CHECK(std::fabs(lag - D) <= tol, "fs %g f %d lag %g declared %g", fs, f, lag, D);
                CHECK(std::fabs(20 * std::log10(amp / 0.5)) <= 0.01, "fs %g f %d gain %g dB", fs, f, 20 * std::log10(amp / 0.5));
                // Dry path: exact integer delay.
                bool exact = true;
                const auto x = sine(N / 4, 0.5, w);
                for (std::size_t n = std::size_t(D); n < x.size(); ++n) exact = exact && dry[n] == x[n - std::size_t(D)];
                CHECK(exact, "dry delay not exact at fs %g f %d", fs, f);
            }
        // Full engine (Studio): impulse peak and 1 kHz lag equal the declared total latency.
        Engine eng; std::string e;
        CHECK(eng.prepare(dividerEngine(fs, {}), e), "engine prepare %s", e.c_str());
        const int D = eng.latencySamples();
        std::vector<std::vector<double>> x(1, std::vector<double>(4096, 0.0)), y;
        x[0][100] = 0.5;
        runEngine(eng, x, 333, y);
        const auto pk = std::max_element(y[0].begin(), y[0].end(), [](double a, double b) { return std::fabs(a) < std::fabs(b); }) - y[0].begin();
        Engine eng2; eng2.prepare(dividerEngine(fs, {}), e);
        std::vector<std::vector<double>> xs(1, sine(std::size_t(fs / 2), 0.5, w)), ys;
        runEngine(eng2, xs, 512, ys);
        double amp, lag; toneFit(ys[0], w, std::size_t(fs / 8), std::size_t(fs / 8) + std::size_t(fs / 10), amp, lag, fs / 1000.0);
        lag = unwrap(lag, D, fs / 1000.0);
        std::printf("P03 engine Studio fs %6.0f: declared %d (circuit %d + clip %d), impulse peak %ld, 1 kHz lag %.7f\n", fs, D,
                    eng.quality().circuitLatency, eng.quality().clipLatency, long(pk) - 100, lag);
        CHECK(pk - 100 == D, "engine impulse peak %ld declared %d", long(pk) - 100, D);
        CHECK(std::fabs(lag - D) <= 1e-6, "engine lag %g declared %d", lag, D);
    }
    // Zero Latency mode: declared 0, impulse lands on the same sample.
    QualitySettings zl; zl.zeroLatency = true;
    Engine eng; std::string e;
    CHECK(eng.prepare(dividerEngine(48000, zl), e) && eng.latencySamples() == 0 && eng.quality().draft, "zero latency %d", eng.latencySamples());
    std::vector<std::vector<double>> x(1, std::vector<double>(256, 0.0)), y;
    x[0][10] = 0.5;
    runEngine(eng, x, 64, y);
    CHECK(std::fabs(y[0][10] - 0.5) < 1e-12 && std::fabs(y[0][11]) < 1e-12, "zero latency impulse %g", y[0][10]);
}

// ---- Block partitions, zero-frame calls, thermal clock independence --------------------------
EngineConfig clipperEngine(double fs, int channels, QualitySettings q = {}) {
    EngineConfig c;
    c.sampleRate = fs; c.maxBlock = 512; c.channels = channels; c.quality = q;
    c.circuit = syntheticDiodeClipper();
    c.initial.autoGain = true;
    return c;
}

std::vector<std::vector<double>> testSignal(int C, std::size_t N, double fs, uint64_t seed) {
    std::vector<std::vector<double>> x(std::size_t(C), std::vector<double>(N, 0.0));
    Lcg g{seed};
    for (int c = 0; c < C; ++c)
        for (std::size_t n = 0; n < N; ++n)
            x[c][n] = 0.6 * std::sin(2 * M_PI * (220.0 + 110 * c) * double(n) / fs) + 0.05 * g.next();
    return x;
}

void p03_partitions() {
    const double fs = 48000;
    const std::size_t N = 24000;
    const auto x = testSignal(2, N, fs, 11);
    std::vector<std::pair<int64_t, ParamEvent>> events = {
        {0, {0, ParamTarget::InputGainDb, 6.0}},   {1000, {0, ParamTarget::Mix, 0.5}},
        {4097, {0, ParamTarget::HpfCutoffHz, 120}}, {9000, {0, ParamTarget::OutputTrimDb, -3}},
        {12345, {0, ParamTarget::HpfEnabled, 0}},   {16000, {0, ParamTarget::SoftClipEnabled, 0}},
        {20000, {0, ParamTarget::HostBypass, 1}},   {22000, {0, ParamTarget::HostBypass, 0}}};
    Engine ref; std::string e;
    CHECK(ref.prepare(clipperEngine(fs, 2), e), "ref prepare %s", e.c_str());
    std::vector<std::vector<double>> yr;
    runEngine(ref, x, 512, yr, &events);
    const auto rd = ref.diagnostics();
    const double tRef = ref.active()->circuit().temperatureK(0, 0);
    for (int B : {1, 17, 64, 127, 512, 8192}) {
        Engine eng; eng.prepare(clipperEngine(fs, 2), e);
        std::vector<std::vector<double>> y;
        runEngine(eng, x, B, y, &events, B == 17 || B == 127 ? 3 : 1);
        double worst = 0;
        for (int c = 0; c < 2; ++c) for (std::size_t n = 0; n < N; ++n) worst = std::max(worst, std::fabs(y[c][n] - yr[c][n]));
        const auto d = eng.diagnostics();
        const double t = eng.active()->circuit().temperatureK(0, 0);
        std::printf("P03 partition %4d (+zero-frame calls): max diff %.3g, samples %lld, thermal ticks %ld, T %.12f K\n", B, worst,
                    (long long)d.samples, d.circuit.thermalTicks, t);
        CHECK(worst <= 1e-12, "partition %d diff %g", B, worst);
        CHECK(d.samples == int64_t(N) && d.circuit.thermalTicks == rd.circuit.thermalTicks && t == tRef &&
              d.circuit.thermalPhase == rd.circuit.thermalPhase, "partition %d clocks", B);
    }
    CHECK(rd.circuit.thermalTicks == long(N * 1000 / 48000), "ticks %ld", rd.circuit.thermalTicks);
    // Zero-frame call alone advances nothing.
    Engine z; z.prepare(clipperEngine(fs, 2), e);
    ProcessBlock zb; zb.ctx = {fs, 0, 0, 0, false}; zb.buses.channels = 2;
    ParamEvent ev{0, ParamTarget::Mix, 0.25}; zb.events = &ev; zb.eventCount = 1;
    const auto r = z.process(zb);
    CHECK(r.frames == 0 && z.diagnostics().samples == 0 && z.diagnostics().circuit.thermalTicks == 0 &&
          z.active()->output().params().mix == 0.25, "zero-frame call");
}

// ---- Stereo equals per-channel mono (auto-gain off: no linked stage) --------------------------
void p03_stereo_mono() {
    const double fs = 48000;
    const auto x = testSignal(2, 12000, fs, 5);
    auto cfg2 = clipperEngine(fs, 2); cfg2.initial.autoGain = false;
    auto cfg1 = clipperEngine(fs, 1); cfg1.initial.autoGain = false;
    Engine s, l, r; std::string e;
    s.prepare(cfg2, e); l.prepare(cfg1, e); r.prepare(cfg1, e);
    std::vector<std::vector<double>> ys, yl, yr;
    runEngine(s, x, 300, ys);
    runEngine(l, {x[0]}, 300, yl);
    runEngine(r, {x[1]}, 300, yr);
    CHECK(ys[0] == yl[0] && ys[1] == yr[0], "stereo channels differ from mono runs");
}

// ---- Sample rates ------------------------------------------------------------------------------
void p03_sample_rates() {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        Engine eng; std::string e;
        CHECK(eng.prepare(clipperEngine(fs, 2), e), "fs %g: %s", fs, e.c_str());
        const std::size_t N = std::size_t(fs / 4);
        std::vector<std::vector<double>> y;
        runEngine(eng, testSignal(2, N, fs, 3), 256, y);
        bool finite = true; double peak = 0;
        for (auto& ch : y) for (double v : ch) { finite = finite && std::isfinite(v); peak = std::max(peak, std::fabs(v)); }
        const auto d = eng.diagnostics();
        const long expectTicks = long(double(N) * 1000.0 / fs);
        std::printf("P03 fs %6.0f Studio clipper: peak %.4f, failed substeps %ld, ticks %ld (expect %ld), T rise %.3g K\n", fs, peak,
                    d.circuit.failedSubsteps, d.circuit.thermalTicks, expectTicks, eng.active()->circuit().temperatureK(0, 0) - 298.15);
        CHECK(finite && peak <= 8.0 && d.circuit.failedSubsteps == 0, "fs %g finite/peak/failures", fs);
        CHECK(d.circuit.thermalTicks == expectTicks, "fs %g ticks %ld", fs, d.circuit.thermalTicks);
        ProcessBlock bad; bad.ctx = {fs * 2, 0, 0, 0, false}; bad.buses.channels = 2;
        CHECK(eng.process(bad).status == status::kRateMismatch, "rate mismatch not flagged");
    }
    CircuitRuntime rt; std::string e;
    CHECK(!rt.prepare(withRate(syntheticDiodeClipper(), 22050, 4, dsp::OsFamily::Fir, true), e), "22.05 kHz accepted");
    CHECK(!rt.prepare(withRate(syntheticDiodeClipper(), 384000, 4, dsp::OsFamily::Fir, true), e), "384 kHz accepted");
    CHECK(!rt.prepare(withRate(syntheticDiodeClipper(), 48000.5, 4, dsp::OsFamily::Fir, true), e), "non-integer rate accepted");
}

// ---- Thermal: synthetic dissipative contract and 1 kHz fractional clock (T20 fixture) ----------
CircuitConfig oneOhmLoad() {
    CircuitConfig c;
    c.netlist.nodes = 1;
    dsp::NetElement v; v.kind = dsp::NetElement::Kind::VoltageSource; v.a = 1; v.b = 0;
    dsp::NetElement r; r.model = dsp::findSyntheticFixture("fixture.resistor"); r.a = 1; r.b = 0; r.param[0] = 1.0;
    c.netlist.elements = {v, r};
    c.io = {0, 1.0, 1, 1.0};
    return c;  // default thermal: C=2 J/K, G=0.2 W/K, ambient 298.15 K (T20 synthetic fixture)
}

void p03_thermal() {
    for (double fs : {48000.0, 44100.0}) {
        CircuitRuntime rt; std::string e;
        CHECK(rt.prepare(withRate(oneOhmLoad(), fs, 1, dsp::OsFamily::Fir, false), e), "load prepare %s", e.c_str());
        const std::vector<double> x(std::size_t(10 * fs), 1.0);  // 1 V across 1 ohm = 1 W (synthetic)
        runCircuit(rt, x, 509);
        const double T = rt.temperatureK(0, 0);
        double tr = 298.15;
        for (int k = 0; k < 10000; ++k) tr = (2.0 / 1e-3 * tr + 1.0 + 0.2 * 298.15) / (2.0 / 1e-3 + 0.2);
        const double analytic = 298.15 + 5.0 * (1 - std::exp(-1.0));
        std::printf("P03 thermal fs %.0f: ticks %ld, P %.6f W, T-Tamb %.6f K, vs analytic %.3g K, vs 1 kHz BE %.3g K\n", fs,
                    rt.diagnostics().thermalTicks, rt.lastPowerW(0, 0), T - 298.15, T - analytic, T - tr);
        CHECK(rt.diagnostics().thermalTicks == 10000, "ticks %ld", rt.diagnostics().thermalTicks);
        CHECK(std::fabs(T - analytic) <= 0.002, "T20 analytic error %g", T - analytic);
        CHECK(std::fabs(T - tr) <= 1e-9, "fractional split error %g (whole-sample binning would be ~1e-2)", T - tr);
    }
    // Only dissipative static branches may be mapped; capacitor energy is never heat.
    auto rc = withRate(syntheticRcLowpass(1000, 1e-8), 48000, 1, dsp::OsFamily::Fir, false);
    CircuitRuntime rt; std::string e;
    auto bad = rc; bad.thermal.elementToNode = {-1, 0, 0};
    CHECK(!rt.prepare(bad, e), "capacitor mapped to heat accepted");
    bad.thermal.elementToNode = {0, -1, -1}; CHECK(!rt.prepare(bad, e), "source mapped to heat accepted");
    bad.thermal.elementToNode = {-1, 1, -1}; CHECK(!rt.prepare(bad, e), "thermal node out of range accepted");
    bad.thermal.elementToNode = {-1, 0}; CHECK(!rt.prepare(bad, e), "short thermal map accepted");
    CHECK(rt.prepare(rc, e), "rc thermal prepare %s", e.c_str());
    // Reported power equals resistor dissipation (u-v)^2/R from the independent BE reference.
    Lcg g{9}; std::vector<double> x(4000); for (double& v : x) v = g.next();
    const double a = (1.0 / 48000) / (1000 * 1e-8);
    double v = 0, worst = 0;
    std::vector<double> w(1), d(1);
    for (double u : x) {
        const double* in[1] = {&u}; double* wo[1] = {w.data()}; double* dz[1] = {d.data()};
        rt.process(in, wo, dz, 1);
        v = (v + a * u) / (1 + a);
        const double p = (u - v) * (u - v) / 1000.0;
        worst = std::max(worst, std::fabs(rt.lastPowerW(0, 0) - p) / (p + 1e-12));
    }
    CHECK(worst <= 1e-6, "RC dissipated power relative error %g", worst);
}

// ---- Solver failure policy: latch, 10 ms fade to aligned dry, explicit reset -----------------
void p03_failure_fallback() {
    const double fs = 48000;
    auto cfg = withRate(syntheticDiodeClipper(), fs, 4, dsp::OsFamily::Fir, true);
    cfg.maxNewton = 1;  // hostile: one Newton iteration can never satisfy the acceptance test on a change
    CircuitRuntime rt; std::string e;
    CHECK(rt.prepare(cfg, e), "prepare %s", e.c_str());
    const auto silence = std::vector<double>(480, 0.0);
    runCircuit(rt, silence, 64);
    CHECK(rt.diagnostics().failedSubsteps == 0 && !rt.diagnostics().fallbackLatched, "silence failed");
    std::vector<double> step(2000, 1.0), dry;
    const auto wet = runCircuit(rt, step, 100, &dry);
    const auto d = rt.diagnostics();
    // First failing sample: index 0 of the step after the pad (circuit sees the step after latency).
    std::size_t latchAt = 0;
    {
        CircuitRuntime probe; probe.prepare(cfg, e);
        runCircuit(probe, silence, 64);
        std::vector<double> one(1, 1.0), w1, d1;
        for (std::size_t k = 0; k < step.size(); ++k) {
            runCircuit(probe, one, 1);
            if (probe.diagnostics().fallbackLatched) { latchAt = k; break; }
        }
    }
    const std::size_t fadeLen = 480;
    bool finite = true, exactDry = true;
    for (std::size_t n = 0; n < wet.size(); ++n) {
        finite = finite && std::isfinite(wet[n]);
        if (n >= latchAt + fadeLen) exactDry = exactDry && wet[n] == dry[n];
    }
    const double mid = wet[latchAt + fadeLen / 2 - 1];
    std::printf("P03 fallback: failed substeps %ld, latched at step sample %zu, weight %.3f, mid-fade wet %.4f dry %.4f\n",
                d.failedSubsteps, latchAt, d.fallbackWeight, mid, dry[latchAt + fadeLen / 2 - 1]);
    CHECK(d.failedSubsteps >= 3 && d.fallbackLatched && d.resetRequested && d.fallbackWeight == 1.0, "latch");
    CHECK(finite && exactDry, "fallback output finite %d exact dry after 10 ms %d", finite, exactDry);
    // Latch persists although the solver may succeed again (needsFallback() clears on success).
    runCircuit(rt, std::vector<double>(2000, 1.0), 100);
    CHECK(rt.diagnostics().fallbackLatched, "latch cleared without reset");
    // Explicit reset: re-entry from the prepared DC state; diagnostics count preserved.
    const long kept = rt.diagnostics().failedSubsteps;
    rt.reset();
    CHECK(!rt.diagnostics().fallbackLatched && !rt.diagnostics().resetRequested && rt.diagnostics().failedSubsteps == kept, "reset");
    CircuitRuntime fresh; fresh.prepare(cfg, e);
    const auto a = runCircuit(rt, silence, 64), b = runCircuit(fresh, silence, 64);
    CHECK(a == b && rt.diagnostics().failedSubsteps == kept, "post-reset silence differs from fresh");

    // Engine-level: same hostile solver, step input -> flags, finite bounded output.
    EngineConfig ec = clipperEngine(fs, 2); ec.maxNewtonOverride = 1;
    Engine eng; CHECK(eng.prepare(ec, e), "engine prepare %s", e.c_str());
    std::vector<std::vector<double>> x(2, std::vector<double>(4800, 0.0)), y;
    for (int c = 0; c < 2; ++c) std::fill(x[c].begin() + 1000, x[c].end(), 0.9);
    runEngine(eng, x, 256, y);
    const auto ed = eng.diagnostics();
    bool ok = true; for (auto& ch : y) for (double v : ch) ok = ok && std::isfinite(v) && std::fabs(v) <= 8.0;
    CHECK(ed.circuit.fallbackLatched && ed.circuit.resetRequested && ok, "engine fallback");
    ProcessBlock zb; zb.ctx = {fs, 0, 0, 0, false}; zb.buses.channels = 2;
    const auto r = eng.process(zb);
    CHECK((r.status & status::kCircuitFallback) && (r.status & status::kResetRequested), "status flags %u", r.status);
    // Hostile huge input at Studio Newton cap: informational.
    Engine h; h.prepare(clipperEngine(fs, 1), e);
    std::vector<std::vector<double>> xh(1, std::vector<double>(4800)), yh;
    for (std::size_t n = 0; n < 4800; ++n) xh[0][n] = 1e5 * std::sin(2 * M_PI * 50 * double(n) / fs);
    runEngine(h, xh, 512, yh);
    bool hf = true; for (double v : yh[0]) hf = hf && std::isfinite(v) && std::fabs(v) <= 8.0;
    std::printf("P03 hostile 1e5 input (Studio cap 8): failed substeps %ld, latched %d, output finite/bounded %d\n",
                h.diagnostics().circuit.failedSubsteps, h.diagnostics().circuit.fallbackLatched, hf);
    CHECK(hf, "hostile output not contained");
}

// ---- Failed prepare, stale generations, reclamation --------------------------------------------
void p03_prepare_generations() {
    const double fs = 48000;
    const auto sig = testSignal(1, 3000, fs, 21)[0];
    std::string e;
    // CircuitRuntime: failed prepare keeps the live state.
    CircuitRuntime a, twin;
    auto cfg = withRate(syntheticDiodeClipper(), fs, 4, dsp::OsFamily::Fir, true);
    a.prepare(cfg, e); twin.prepare(cfg, e);
    std::vector<double> head(sig.begin(), sig.begin() + 1500), tail(sig.begin() + 1500, sig.end());
    runCircuit(a, head, 100); runCircuit(twin, head, 100);
    auto floating = cfg;  // Vin -R- n3; node 2 (the output) has no DC path: floating
    floating.netlist.elements.resize(2);
    floating.netlist.nodes = 3; floating.netlist.elements[1].b = 3;
    CHECK(!a.prepare(floating, e) && !e.empty(), "floating netlist accepted");
    std::printf("P03 failed prepare reason: %s\n", e.c_str());
    auto badOs = cfg; badOs.os.factor = 3; CHECK(!a.prepare(badOs, e), "factor 3 accepted");
    auto badIo = cfg; badIo.io.sourceElement = 1; CHECK(!a.prepare(badIo, e), "non-source input accepted");
    CHECK(runCircuit(a, tail, 100) == runCircuit(twin, tail, 100), "failed prepare disturbed live circuit");

    OutputStage o; OutputConfig oc; oc.clipOs.factor = 4;
    CHECK(o.prepare(oc, e), "output prepare");
    OutputConfig ob = oc; ob.clipOs.factor = 3; CHECK(!o.prepare(ob, e) && o.prepared() && o.latencySamples() > 0, "output failed prepare");

    // Engine: failed build keeps the active engine; generations; stale rejection; reclamation.
    Engine eng, eTwin;
    CHECK(eng.prepare(clipperEngine(fs, 1), e) && eTwin.prepare(clipperEngine(fs, 1), e), "engine prepare");
    const uint64_t g0 = eng.diagnostics().generation;
    std::vector<std::vector<double>> y1, y2;
    runEngine(eng, {head}, 64, y1); runEngine(eTwin, {head}, 64, y2);
    EngineConfig badQ = clipperEngine(fs, 1); badQ.quality.mode = QualityMode::Manual; badQ.quality.manualFactor = 3;
    CHECK(!eng.prepare(badQ, e) && eng.diagnostics().generation == g0, "bad quality installed");
    EngineConfig badC = clipperEngine(fs, 1); badC.circuit = floating;
    CHECK(!eng.buildPrepared(badC, e), "bad circuit built");
    runEngine(eng, {tail}, 64, y1); runEngine(eTwin, {tail}, 64, y2);
    CHECK(y1 == y2, "failed prepare disturbed live engine");
    auto hA = eng.buildPrepared(clipperEngine(fs, 1), e);
    auto hB = eng.buildPrepared(dividerEngine(fs, {}), e);
    CHECK(hA && hB && hB->generation() > hA->generation(), "generations");
    const PreparedEngine* oldActive = eng.active();
    CHECK(eng.tryInstallPrepared(hB) && hB.get() == oldActive, "install newer; previous returned for reclamation");
    hB.reset();  // off-audio reclamation
    CHECK(!eng.tryInstallPrepared(hA) && hA && eng.diagnostics().staleInstalls == 1, "stale generation installed");
    std::unique_ptr<PreparedEngine> none;
    CHECK(!eng.tryInstallPrepared(none), "null installed");
    ProcessBlock nb; nb.ctx = {fs, 0, 0, 0, false}; nb.buses.channels = 2;
    CHECK(eng.process(nb).status == status::kLayoutMismatch, "layout mismatch not flagged");
    Engine unprepared; CHECK(unprepared.process(nb).status == status::kNotPrepared, "unprepared not flagged");
}

// ---- Manifest lifetime: runtime owns copies (run under ASan) --------------------------------
void p03_manifest_lifetime() {
    const double fs = 48000;
    const auto sig = testSignal(1, 4000, fs, 2)[0];
    auto parsed = [](int idx) {
        core::JsonValue j; std::string e;
        auto m = std::make_unique<dsp::DeviceManifest>();
        if (!core::parseJson(dsp::syntheticFixtureJson(idx), j, e) || !dsp::parseDeviceManifest(j, *m, e)) return std::unique_ptr<dsp::DeviceManifest>();
        return m;
    };
    auto res = parsed(0), cap = parsed(1), dio = parsed(2);
    CHECK(res && cap && dio && dio->modelId == "fixture.diode", "parse fixtures");
    auto cfg = withRate(syntheticDiodeClipper(), fs, 4, dsp::OsFamily::Fir, true);
    cfg.netlist.elements[1].model = res.get();
    cfg.netlist.elements[2].model = dio.get(); cfg.netlist.elements[3].model = dio.get();
    cfg.netlist.elements[4].model = cap.get();
    CircuitRuntime rt, ref; std::string e;
    CHECK(rt.prepare(cfg, e) && rt.ownedManifestCount() == 3, "prepare owned %d", rt.ownedManifestCount());
    ref.prepare(withRate(syntheticDiodeClipper(), fs, 4, dsp::OsFamily::Fir, true), e);
    // Scribble then destroy the caller's manifests and netlist before processing.
    dio->params[0].min = -1; dio->maxExpArg = -5; dio->modelId = "garbage";
    res.reset(); cap.reset(); dio.reset();
    cfg.netlist.elements.clear();
    CHECK(runCircuit(rt, sig, 128) == runCircuit(ref, sig, 128), "output changed after manifests destroyed");
    // Replacement: prepare again from a new temporary manifest set that is destroyed immediately.
    {
        auto r2 = parsed(0);
        auto c2 = withRate(syntheticRcLowpass(1000, 1e-8), fs, 4, dsp::OsFamily::Fir, true);
        c2.netlist.elements[1].model = r2.get();
        CHECK(rt.prepare(c2, e), "replace prepare");
    }
    CircuitRuntime rcRef; rcRef.prepare(withRate(syntheticRcLowpass(1000, 1e-8), fs, 4, dsp::OsFamily::Fir, true), e);
    CHECK(runCircuit(rt, sig, 128) == runCircuit(rcRef, sig, 128), "replaced manifest output differs");
}

// ---- Reset reproducibility ------------------------------------------------------------------
void p03_reset_reproducibility() {
    const double fs = 48000;
    const auto x = testSignal(2, 9000, fs, 4);
    Engine eng; std::string e;
    eng.prepare(clipperEngine(fs, 2), e);
    std::vector<std::vector<double>> a, b;
    runEngine(eng, x, 256, a);
    const double t1 = eng.active()->circuit().temperatureK(1, 0);
    eng.reset();
    runEngine(eng, x, 256, b);
    CHECK(a == b && eng.active()->circuit().temperatureK(1, 0) == t1, "reset not reproducible");
    CHECK(eng.diagnostics().samples == 18000, "sample counter %lld", (long long)eng.diagnostics().samples);
}

// ---- T21 re-measured with the synthetic clipper inside the oversampler ------------------------
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> w = std::polar(1.0, -2 * M_PI / double(len));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> wk = 1;
            for (std::size_t k = 0; k < len / 2; ++k, wk *= w) {
                const auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
        }
    }
}

std::vector<double> circuitSpectrum(int factor, dsp::OsFamily fam, double stopDb, int newton, double amp, long& failures) {
    const int N = 16384, bins[5] = {7 * 37, 7 * 101, 7 * 233, 7 * 419, 7 * 677};
    auto cfg = withRate(syntheticDiodeClipper(), 48000, factor, fam, false, 256);
    cfg.os.stopbandDb = stopDb; cfg.maxNewton = newton; cfg.thermal.enabled = false;
    CircuitRuntime rt; std::string e;
    if (!rt.prepare(cfg, e)) { std::printf("T21 prepare failed: %s\n", e.c_str()); ++g_fail; return {}; }
    std::vector<double> x(std::size_t(3 * N));
    for (std::size_t n = 0; n < x.size(); ++n) {
        double v = 0;
        for (int b : bins) v += amp * std::sin(2 * M_PI * double(b) * double(n) / N);
        x[n] = v;
    }
    const auto y = runCircuit(rt, x, 256);
    failures = rt.diagnostics().failedSubsteps;
    std::vector<std::complex<double>> X(y.begin() + 2 * N, y.end());
    fft(X);
    std::vector<double> p(N / 2);
    for (int k = 0; k < N / 2; ++k) p[std::size_t(k)] = std::norm(X[std::size_t(k)]);
    return p;
}

void t21_integrated_alias() {
    // Method = docs/TRACEABILITY.md T21 (Fs 48 kHz, N=16384, bins 7*{37,101,233,419,677}, steady
    // state 2N..3N-1, unwanted = in-band bins not divisible by 7 vs in-band power of the 64x FIR
    // 140 dB reference) with tanh replaced by the SYNTHETIC diode clipper (1 V per unit input).
    // Stimuli: nominal 0.1 V per tone (0.5 V peak, onset of conduction), hot 0.3 V per tone
    // (1.5 V peak, hard diode clipping), extreme 1.0 V per tone (5 V peak, information only).
    // Gated: nominal and hot at Studio 4x (-70 dB) and Render 16x (-85 dB) for THIS synthetic
    // circuit and stimulus only. Extreme and Draft/IIR rows are recorded, not gated.
    const int N = 16384;
    const double df = 48000.0 / N;
    const int k0 = int(std::ceil(20.0 / df)), k1 = int(20000.0 / df);
    auto unwanted = [&](const std::vector<double>& p) { double s = 0; for (int k = k0; k <= k1; ++k) if (k % 7) s += p[std::size_t(k)]; return s; };
    struct Mode { const char* name; int f; dsp::OsFamily fam; double spec; };
    for (const auto& st : {std::pair<const char*, double>{"nominal", 0.1}, {"hot", 0.3}, {"extreme", 1.0}}) {
        long fr = 0;
        const auto pr = circuitSpectrum(64, dsp::OsFamily::Fir, 140.0, 12, st.second, fr);
        if (pr.empty()) return;
        double eRef = 0; for (int k = k0; k <= k1; ++k) eRef += pr[std::size_t(k)];
        const double self = 10 * std::log10(unwanted(pr) / eRef + 1e-300);
        std::printf("T21 circuit %-7s 64x reference self-alias %8.2f dB (failed substeps %ld)\n", st.first, self, fr);
        CHECK(self <= -120.0 && fr == 0, "reference self-alias %g dB", self);
        for (const Mode m : {Mode{"Studio 4x FIR", 4, dsp::OsFamily::Fir, -70}, Mode{"Render 16x FIR", 16, dsp::OsFamily::Fir, -85},
                             Mode{"4x IIR", 4, dsp::OsFamily::Iir, 0}, Mode{"2x FIR Draft", 2, dsp::OsFamily::Fir, 0}}) {
            long f = 0;
            const auto p = circuitSpectrum(m.f, m.fam, 100.0, m.f >= 16 ? 12 : 8, st.second, f);
            const double a = 10 * std::log10(unwanted(p) / eRef + 1e-300);
            std::printf("T21 circuit %-7s %-15s alias %8.2f dB (%s %.0f dB)%s failed substeps %ld\n", st.first, m.name, a,
                        m.spec < 0 ? "proposed" : "no spec", m.spec, (m.spec < 0 && a > m.spec) ? " FINDING: misses proposed target," : "", f);
            const bool gated = m.spec < 0 && std::string(st.first) != "extreme";
            if (gated) {
                CHECK(f == 0, "%s %s solver failures %ld invalidate the measurement", st.first, m.name, f);
                CHECK(a <= m.spec, "%s %s alias %g dB exceeds proposed %g dB", st.first, m.name, a, m.spec);
            } else if (f != 0) {
                std::printf("T21 note: %s %s ran with %ld failed substeps (held state); value is informational only\n", st.first, m.name, f);
            }
        }
    }
}

}  // namespace

int main() {
    p03_latency_plan();
    p03_rc_reference();
    p03_diode_reference();
    p03_measured_latency();
    p03_partitions();
    p03_stereo_mono();
    p03_sample_rates();
    p03_thermal();
    p03_failure_fallback();
    p03_prepare_generations();
    p03_manifest_lifetime();
    p03_reset_reproducibility();
    t21_integrated_alias();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
