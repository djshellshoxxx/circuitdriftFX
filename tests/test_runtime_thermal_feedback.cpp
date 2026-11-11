// CircuitDrift tests: temperature feedback (resume packet 2) and Drift creative motion wiring
// (02-musical "Actual thermal state versus creative stochastic drift"; T06 Drift extension).
// The tempco resistor (fixture.resistor.tempco), thermal node, creative filter, pitch-drift delay
// and Analog Noise are labelled SYNTHETIC / creative fixtures; nothing here validates hardware or a
// measured tempco (G01 open). Owner: engine2 (runtime).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <span>
#include <string>
#include <vector>
#include "core/Json.h"
#include "dsp/DeviceManifest.h"
#include "dsp/MnaSolver.h"
#include "runtime/CircuitRuntime.h"
#include "runtime/Engine.h"
#include "state/TransactionQueue.h"
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::runtime;
using cd::state::TransactionEvent;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)
long g_rtCount = 0;
constexpr double kFs = 48000;
constexpr double kR0 = 100.0, kAlpha = 3.9e-3, kTref = 298.15;
using Buf = std::vector<std::vector<double>>;

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
};

dsp::NetElement device(const char* id, int a, int b, double v) {
    dsp::NetElement e;
    e.model = dsp::findSyntheticFixture(id);
    e.a = a; e.b = b; e.param[0] = v;
    return e;
}

// SYNTHETIC divider: Vin -R1(tempco, thermal node 0)- n2 -R2- 0; 20 V per host unit, out = V(n2)/20.
CircuitConfig tempcoDivider() {
    CircuitConfig c;
    c.netlist.nodes = 2;
    dsp::NetElement vs;
    vs.kind = dsp::NetElement::Kind::VoltageSource; vs.a = 1; vs.b = 0;
    c.netlist.elements = {vs, device("fixture.resistor.tempco", 1, 2, kR0), device("fixture.resistor", 2, 0, kR0)};
    c.io = {0, 20.0, 2, 1.0 / 20.0};
    c.thermal.network.n = 1;
    c.thermal.network.capacity[0] = 0.05;   // J/K (synthetic)
    c.thermal.network.toAmbient[0] = 0.01;  // W/K (synthetic) -> tau 5 s
    c.thermal.elementToNode = {-1, 0, -1};
    return c;
}

double law(double t, double k) { return kR0 * (1.0 + k * kAlpha * (t - kTref)); }

// ---------------------------------------------------------------------------------------------
void manifestLaw() {
    const dsp::DeviceManifest* m = dsp::findSyntheticFixture("fixture.resistor.tempco");
    CHECK(m && m->temperature.present && m->temperature.paramIndex == 0 && m->temperature.tempcoPerK == kAlpha &&
              m->temperature.referenceK == kTref && m->provenance == dsp::ProvenanceKind::SyntheticFixture,
          "tempco fixture");
    CHECK(dsp::syntheticFixtureCatalogue().size() == 3 && !dsp::findSyntheticFixture("fixture.resistor")->temperature.present,
          "original catalogue unchanged");
    bool cl = false;
    CHECK(m->temperature.evaluate(kR0, 348.15, 1.0, 1e-3, 1e9, cl) == law(348.15, 1.0) && !cl, "law at 75 C");
    const double hot = m->temperature.evaluate(kR0, 1000.0, 1.0, 1e-3, 1e9, cl);
    CHECK(cl && hot == law(473.15, 1.0), "T clamped to the model domain");
    m->temperature.evaluate(kR0, 1000.0, 1e6, 1e-3, 150.0, cl);
    CHECK(cl, "value clamped to parameter bounds");
    CHECK(m->temperature.evaluate(kR0, std::nan(""), 1.0, 1e-3, 1e9, cl) == kR0 && cl, "NaN T -> reference");
    // Schema mutations rejected.
    const std::string base = dsp::syntheticThermalFixtureJson(0);
    const std::pair<const char*, const char*> muts[] = {
        {"\"tempcoPerK\":3.9e-3", "\"tempcoPerK\":0.5"},
        {"\"minK\":233.15", "\"minK\":300"},
        {"\"parameter\":\"resistanceOhm\",\"tempco", "\"parameter\":\"ohms\",\"tempco"},
        {"\"maxK\":473.15}", "\"maxK\":473.15,\"slope\":1}"},
        {"\"referenceK\":298.15", "\"referenceK\":-1"},
    };
    int rejected = 0;
    for (const auto& mu : muts) {
        std::string t = base;
        const auto at = t.find(mu.first);
        CHECK(at != std::string::npos, "mutation anchor %s", mu.first);
        if (at == std::string::npos) continue;
        t.replace(at, std::string(mu.first).size(), mu.second);
        core::JsonValue j;
        std::string e;
        dsp::DeviceManifest out;
        if (core::parseJson(t, j, e) && !dsp::parseDeviceManifest(j, out, e)) ++rejected;
    }
    CHECK(rejected == 5, "temperature law mutations rejected %d/5", rejected);
    std::printf("Temperature law (SYNTHETIC fixture.resistor.tempco, alpha %.1e/K): analytic evaluation, domain/bounds clamps, %d/5 "
                "schema mutations rejected; original 3-fixture catalogue unchanged\n", kAlpha, rejected);
}

void solverApi() {
    std::string e;
    CircuitConfig c = tempcoDivider();
    dsp::Netlist net = c.netlist;
    net.elements[0].value = 10.0;
    net.elements.push_back(device("fixture.capacitor", 2, 0, 1e-6));
    dsp::MnaSolver s;
    CHECK(s.prepare(net, 1.0 / kFs, {8, 1e-7}, e), "prepare %s", e.c_str());
    // Rejections leave the value unchanged.
    const double r = s.elementParameter(1, 0);
    CHECK(!s.setElementParameter(1, 0, std::nan("")) && !s.setElementParameter(1, 0, INFINITY) && !s.setElementParameter(1, 0, -5) &&
              !s.setElementParameter(1, 0, 0.0) && !s.setElementParameter(1, 0, 2e9) && !s.setElementParameter(0, 0, 50) &&
              !s.setElementParameter(1, 1, 50) && !s.setElementParameter(-1, 0, 50) && !s.setElementParameter(99, 0, 50) &&
              s.elementParameter(1, 0) == r && std::isnan(s.elementParameter(0, 0)) && std::isnan(s.elementParameter(1, 3)),
          "setElementParameter domain validation");
    // Accepted update: the next steps converge to the analytic divider.
    int it = 0;
    {
        cd::test::RtTrapScope trap;
        CHECK(s.setElementParameter(1, 0, 300.0), "accept 300 Ohm");
        for (; it < 20000; ++it) s.step();
        g_rtCount += trap.counts().allocations + trap.counts().deallocations;
    }
    const double v = s.voltage(2), expect = 10.0 * kR0 / (300.0 + kR0);
    CHECK(std::fabs(v - expect) <= 1e-9, "divider after update %.12f vs %.12f", v, expect);
    // Capacitor update keeps the stored charge (charge conservation): q_old unchanged by the update.
    std::vector<double> st(std::size_t(s.stateSize())), st2(st.size());
    s.getState(st.data());
    CHECK(s.setElementParameter(3, 0, 2e-6), "capacitor update");
    s.getState(st2.data());
    CHECK(st == st2, "capacitor update changed stored state");
    std::printf("MnaSolver::setElementParameter: 9 invalid updates rejected unchanged; 100->300 Ohm divider settles to analytic "
                "(|err| %.1e V); capacitor update keeps q_old\n", std::fabs(v - expect));
}

// Analytic check of R(T), one-tick lag and an independent thermal reference integration.
void feedbackAnalytic() {
    std::string e;
    CircuitConfig c = tempcoDivider();
    c.sampleRate = kFs; c.maxBlock = 64; c.channels = 1;
    c.os.factor = 1; c.padToFamilyMax = false;
    CircuitRuntime cr;
    CHECK(cr.prepare(c, e), "prepare %s", e.c_str());
    CHECK(cr.hasTemperatureFeedback() && cr.diagnostics().feedbackElements == 1, "feedback element detected");
    const double vin = 0.5 * 20.0;
    // Independent reference: per-sample power with the R in force for that sample, ticks every 48 samples.
    const double C = 0.05, G = 0.01, dt = 1e-3, tAmb = 298.15;
    double tRef = tAmb, rRef = law(tAmb, 1.0), eAcc = 0;
    double worstR = 0, worstV = 0, worstT = 0;
    int lagViolations = 0;
    const double x = 0.5;
    double y = 0, w = 0, dr = 0;
    const double* in[1] = {&x};
    double* wp[1] = {&w};
    double* dp[1] = {&dr};
    for (int i = 0; i < int(kFs); ++i) {
        const double rUsed = cr.elementParameter(0, 1, 0);   // parameter in force for this sample
        const std::int64_t ticks0 = cr.diagnostics().thermalTicks;
        {
            cd::test::RtTrapScope trap;
            cr.process(in, wp, dp, 1);
            g_rtCount += trap.counts().allocations + trap.counts().deallocations;
        }
        y = cr.voltage(0, 2);
        const double vExpect = vin * kR0 / (rUsed + kR0);
        worstV = std::max(worstV, std::fabs(y - vExpect) / vExpect);
        if (std::fabs(rUsed / rRef - 1.0) > 1e-12) ++lagViolations;   // solved with the value of the previous tick
        // reference integration
        const double i1 = vin / (rRef + kR0);
        eAcc += i1 * i1 * rRef / kFs;
        if ((i + 1) % 48 == 0) {
            const double p = eAcc / dt;
            eAcc = 0;
            tRef = (C / dt * tRef + p + G * tAmb) / (C / dt + G);
            rRef = law(tRef, 1.0);
        }
        if (cr.diagnostics().thermalTicks != ticks0) {
            const double t = cr.temperatureK(0, 0), rNow = cr.elementParameter(0, 1, 0);
            worstR = std::max(worstR, std::fabs(rNow / law(t, 1.0) - 1.0));
            worstT = std::max(worstT, std::fabs(t - tRef));
        }
    }
    const double tEnd = cr.temperatureK(0, 0), rEnd = cr.elementParameter(0, 1, 0);
    CHECK(worstR <= 1e-15, "R(T) vs analytic law rel %.3e", worstR);
    CHECK(worstV <= 1e-9, "divider voltage vs analytic with the in-force R rel %.3e", worstV);
    CHECK(lagViolations == 0, "one-tick lag violated %d times", lagViolations);
    CHECK(worstT <= 1e-9 && tEnd - tAmb > 2.0, "thermal vs independent reference %.3e K (rise %.3f K)", worstT, tEnd - tAmb);
    CHECK(cr.diagnostics().feedbackUpdates > 900 && cr.diagnostics().feedbackRejected == 0, "feedback updates %lld",
          (long long)cr.diagnostics().feedbackUpdates);
    std::printf("Thermal feedback (SYNTHETIC divider, 10 V, C 0.05 J/K, G 0.01 W/K, 1 s): T rise %.4f K, R1 %.6f -> %.6f Ohm; R(T) vs "
                "law rel %.1e; V(out) vs analytic (R in force) rel %.1e; T vs independent BE reference %.1e K; one-tick lag: every "
                "sample solved with the value of the previous tick (0 violations)\n",
                tEnd - tAmb, law(tAmb, 1.0), rEnd, worstR, worstV, worstT);
}

EngineConfig driftEngine() {
    EngineConfig c;
    c.sampleRate = kFs; c.maxBlock = 512; c.channels = 1;
    c.quality.mode = QualityMode::Manual; c.quality.manualFactor = 1;
    c.circuit = tempcoDivider();
    c.initial.hpfEnabled = false; c.initial.softClipEnabled = false;
    return c;
}

struct Ev { std::size_t t; uint32_t id; double norm; };
Buf render(Engine& eng, const Buf& in, uint32_t part, const std::vector<Ev>& evs = {}) {
    const int C = int(in.size());
    const std::size_t N = in[0].size();
    Buf out(std::size_t(C), std::vector<double>(N, 0.0));
    std::vector<TransactionEvent> be;
    be.reserve(32);
    std::size_t pos = 0, ev = 0;
    while (pos < N) {
        const uint32_t f = uint32_t(std::min<std::size_t>(part, N - pos));
        be.clear();
        while (ev < evs.size() && evs[ev].t < pos + f) { be.push_back(TransactionEvent::param(evs[ev].id, evs[ev].norm, uint32_t(evs[ev].t - pos))); ++ev; }
        const double* ip[2] = {in[0].data() + pos, C > 1 ? in[1].data() + pos : nullptr};
        double* op[2] = {out[0].data() + pos, C > 1 ? out[1].data() + pos : nullptr};
        cd::test::RtTrapScope trap;
        eng.process(ProcessContext{kFs, int64_t(pos), 0, f, false}, AudioBuses{C, ip, op}, std::span<const TransactionEvent>(be.data(), be.size()));
        g_rtCount += trap.counts().allocations + trap.counts().deallocations;
        pos += f;
    }
    return out;
}

double peakDiff(const Buf& a, const Buf& b) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = 0; i < a[c].size(); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}

// T06 extension: Drift multiplies the temperature-law sensitivity by 1+4d.
void driftSensitivity() {
    std::string e;
    const Buf dc(1, std::vector<double>(24000, 0.5));
    double rel[3] = {};
    const double ds[3] = {0.0, 0.5, 1.0};
    for (int k = 0; k < 3; ++k) {
        Engine eng;
        EngineConfig c = driftEngine();
        c.initialMacros.drift = ds[k];
        CHECK(eng.prepare(c, e), "prepare %s", e.c_str());
        render(eng, dc, 480);
        const auto d = eng.diagnostics();
        const double t = eng.active()->circuit().temperatureK(0, 0), r = eng.active()->circuit().elementParameter(0, 1, 0);
        const double kExp = 1.0 + 4.0 * ds[k];
        rel[k] = std::fabs((r / kR0 - 1.0) / (kAlpha * (t - kTref)) / kExp - 1.0);
        CHECK(d.effective.supportsDrift && std::fabs(d.effective.driftSensitivity - kExp) <= 1e-12 && rel[k] <= 1e-9,
              "Drift %.1f: sensitivity %.12f, (R/R0-1)/(alpha dT) rel err %.3e", ds[k], d.effective.driftSensitivity, rel[k]);
    }
    // Via a host event (macro.drift 1002) on a running engine.
    Engine ev;
    CHECK(ev.prepare(driftEngine(), e), "prepare");
    render(ev, dc, 256, {{100, 1002, 1.0}});
    const double t = ev.active()->circuit().temperatureK(0, 0), r = ev.active()->circuit().elementParameter(0, 1, 0);
    const double relEv = std::fabs((r / kR0 - 1.0) / (kAlpha * (t - kTref)) / 5.0 - 1.0);
    CHECK(relEv <= 1e-9, "Drift event sensitivity rel %.3e", relEv);
    std::printf("T06 Drift extension (SYNTHETIC tempco): measured R/R0-1 = (1+4d) alpha dT at d={0,0.5,1}: rel err %.1e %.1e %.1e; "
                "via host event d=1: %.1e (limit 1e-9)\n", rel[0], rel[1], rel[2], relEv);
}

EngineConfig creativeEngine(bool cutoff, bool pitch, uint64_t seed, double drift) {
    EngineConfig c;
    c.sampleRate = kFs; c.maxBlock = 512; c.channels = 2;
    c.quality.mode = QualityMode::Studio;
    c.circuit = syntheticDiodeClipper();
    c.initial.hpfEnabled = false; c.initial.softClipEnabled = false;
    c.creative.cutoffMotion = cutoff; c.creative.cutoffHz = 4000; c.creative.pitchDelay = pitch;
    c.seed = seed;
    c.initialMacros.drift = drift;
    return c;
}

Buf testInput(std::size_t n) {
    Buf x(2, std::vector<double>(n, 0.0));
    Lcg g{17};
    for (std::size_t i = 0; i < n; ++i) {
        const double s = 0.3 * std::sin(2 * M_PI * 440.0 * double(i) / kFs);
        x[0][i] = s + 0.1 * g.d();
        x[1][i] = 0.8 * s + 0.1 * g.d();
    }
    return x;
}

void creativeMotion() {
    std::string e;
    const std::size_t N = 48000;
    const Buf in = testInput(N);
    auto run = [&](const EngineConfig& c, uint32_t part = 480, Engine* keep = nullptr) {
        Engine local;
        Engine& eng = keep ? *keep : local;
        CHECK(eng.prepare(c, e), "prepare %s", e.c_str());
        return render(eng, in, part);
    };
    // d = 0 removes creative motion: bit-identical for every seed (cutoff filter and pitch delay on).
    const Buf a0 = run(creativeEngine(true, true, 1, 0.0)), b0 = run(creativeEngine(true, true, 987654321, 0.0));
    CHECK(peakDiff(a0, b0) == 0.0, "d=0 not seed-independent: %.3e", peakDiff(a0, b0));
    // d = 0 equals the static configuration: cutoff at base and a pure integer pitch delay.
    Engine ref;
    const Buf noPitch = run(creativeEngine(true, false, 1, 0.0), 480, &ref);
    const int dL = 72;   // round(1.5 ms * 48 kHz), exact at 48 kHz
    double shiftDiff = 0;
    for (int c = 0; c < 2; ++c)
        for (std::size_t i = std::size_t(dL); i < N; ++i) shiftDiff = std::max(shiftDiff, std::fabs(a0[std::size_t(c)][i] - noPitch[std::size_t(c)][i - std::size_t(dL)]));
    CHECK(shiftDiff <= 1e-12, "d=0 pitch delay is not the pure declared delay: %.3e", shiftDiff);
    // Seeds reproducible; different seeds differ; motion bounded by +-0.15 d octaves.
    Engine m1;
    const Buf s1 = run(creativeEngine(true, true, 42, 0.8), 480, &m1), s1b = run(creativeEngine(true, true, 42, 0.8));
    const Buf s2 = run(creativeEngine(true, true, 43, 0.8));
    CHECK(peakDiff(s1, s1b) == 0.0 && peakDiff(s1, s2) > 1e-4, "seed reproducibility %.3e / difference %.3e", peakDiff(s1, s1b),
          peakDiff(s1, s2));
    // Partition invariance with motion, pitch delay and noise.
    double part = 0;
    {
        EngineConfig c = creativeEngine(true, true, 42, 0.8);
        c.analogNoise = true; c.initialMacros.age = 0.5;
        const Buf p0 = run(c, 512);
        for (uint32_t p : {1u, 17u, 64u, 127u, 8192u}) part = std::max(part, peakDiff(p0, run(c, p)));
        CHECK(part == 0.0, "creative path partition variance %.3e", part);
    }
    // Cutoff motion range: sampled cutoffs stay inside base * 2^(+-0.15 d).
    double lo = 1e9, hi = 0;
    {
        Engine eng;
        CHECK(eng.prepare(creativeEngine(true, false, 42, 1.0), e), "prepare");
        const Buf x = testInput(480);
        for (int blk = 0; blk < 400; ++blk) {
            render(eng, x, 480);
            const double f = eng.diagnostics().creativeCutoffHz;
            lo = std::min(lo, f); hi = std::max(hi, f);
        }
        CHECK(lo >= 4000 * std::exp2(-0.15) - 1e-9 && hi <= 4000 * std::exp2(0.15) + 1e-9 && hi - lo > 50.0,
              "cutoff motion range %.2f..%.2f Hz", lo, hi);
    }
    // Delay latency reported and dry aligned: at mix 0 (dry endpoint, which still passes the output
    // stage's soft-clip resampling) the pitch-delay engine's output equals the no-delay engine's
    // output delayed by exactly the extra declared 72 samples, and both track the input at their
    // declared latency.
    {
        EngineConfig cp = creativeEngine(false, true, 1, 1.0), cn = creativeEngine(false, false, 1, 1.0);
        cp.initial.mix = 0.0; cn.initial.mix = 0.0;
        Engine ep, en;
        CHECK(ep.prepare(cp, e) && en.prepare(cn, e), "prepare");
        const int L = ep.latencySamples(), Ln = en.latencySamples();
        const auto q = ep.quality();
        CHECK(q.pitchDelay && q.pitchLatency == 72 && q.pitchResidual == 0.0 && L == Ln + 72, "pitch latency %d vs %d", L, Ln);
        const Buf y = render(ep, in, 480), yn = render(en, in, 480);
        double dd = 0, track = 0, trackN = 0;
        for (int c = 0; c < 2; ++c)
            for (std::size_t i = std::size_t(L); i < N; ++i) {
                const auto cc = std::size_t(c);
                dd = std::max(dd, std::fabs(y[cc][i] - yn[cc][i - 72]));
                track = std::max(track, std::fabs(y[cc][i] - in[cc][i - std::size_t(L)]));
                trackN = std::max(trackN, std::fabs(yn[cc][i - 72] - in[cc][i - std::size_t(L)]));
            }
        CHECK(dd <= 1e-12 && track == trackN, "dry not aligned to the declared latency: %.3e (track %.3e vs %.3e)", dd, track, trackN);
        // Zero Latency: pitch delay unavailable, no latency added.
        EngineConfig cz = creativeEngine(false, true, 1, 1.0);
        cz.quality.zeroLatency = true;
        Engine ez;
        CHECK(ez.prepare(cz, e), "prepare ZL");
        CHECK(!ez.quality().pitchDelay && ez.quality().pitchDelayUnavailable && ez.latencySamples() == 0, "ZL pitch delay");
        // 44.1 kHz: fractional base delay reported explicitly.
        EngineConfig c44 = cp;
        c44.sampleRate = 44100;
        Engine e44;
        CHECK(e44.prepare(c44, e), "prepare 44.1");
        std::printf("Pitch-drift delay: +%d samples declared at 48 kHz (residual %.2f), mix-0 output vs no-delay engine shifted 72: |err| %.1e (input tracking at mix 0 limited by the soft-clip stage resampling, identical with and without delay); Zero Latency: unavailable, "
                    "latency 0; 44.1 kHz: %d declared, residual %+.3f samples (reported, not hidden)\n",
                    q.pitchLatency, q.pitchResidual, dd, e44.quality().pitchLatency, e44.quality().pitchResidual);
    }
    std::printf("Creative motion (labelled creative): d=0 bit-identical across seeds (peak %.1e) and equal to the static path + pure "
                "72-sample delay (|err| %.1e); seed 42 reproducible (0), seed 43 differs %.3e; cutoff 4 kHz moved %.1f..%.1f Hz at d=1 "
                "(bound +-0.15 oct); partitions {1,17,64,127,512,8192} diff %.1e\n",
                peakDiff(a0, b0), shiftDiff, peakDiff(s1, s2), lo, hi, part);
}

void analogNoise() {
    std::string e;
    const std::size_t N = 96000;
    const Buf z(2, std::vector<double>(N, 0.0));
    auto cfg = [](double age, bool on, uint64_t seed) {
        EngineConfig c = creativeEngine(false, false, seed, 0.0);
        c.initialMacros.age = age; c.analogNoise = on;
        return c;
    };
    auto rmsDb = [&](const Buf& y) {
        double s = 0;
        for (std::size_t i = 4800; i < N; ++i) s += y[0][i] * y[0][i];
        return 10 * std::log10(s / double(N - 4800));
    };
    Engine off, a0, a1, a1b, a1c;
    CHECK(off.prepare(cfg(1.0, false, 5), e) && a0.prepare(cfg(0.0, true, 5), e) && a1.prepare(cfg(1.0, true, 5), e) &&
              a1b.prepare(cfg(1.0, true, 5), e) && a1c.prepare(cfg(1.0, true, 6), e), "prepare");
    const Buf yo = render(off, z, 480), y0 = render(a0, z, 480), y1 = render(a1, z, 480), y1b = render(a1b, z, 480), y1c = render(a1c, z, 480);
    double offPeak = 0;
    for (double v : yo[0]) offPeak = std::max(offPeak, std::fabs(v));
    const double d0 = rmsDb(y0), d1 = rmsDb(y1);
    CHECK(offPeak == 0.0, "Analog Noise off but output %.3e", offPeak);
    CHECK(std::fabs(d0 + 90.0) <= 0.5 && std::fabs(d1 + 66.0) <= 0.5, "noise levels %.3f / %.3f dBFS", d0, d1);
    CHECK(peakDiff(y1, y1b) == 0.0 && peakDiff(y1, y1c) > 0.0, "noise seed determinism");
    // Host event analogNoise.enabled (1004) turns it on.
    Engine evn;
    CHECK(evn.prepare(cfg(1.0, false, 5), e), "prepare");
    render(evn, z, 480, {{0, 1004, 1.0}});
    CHECK(evn.diagnostics().analogNoise && evn.diagnostics().noiseRms > 0, "noise event");
    std::printf("Analog Noise (seeded, creative): off -> silent (peak %.1e); a=0 %.3f dBFS, a=1 %.3f dBFS (targets -90/-66 +-0.5 dB); "
                "same seed bit-identical, other seed differs; host event 1004 enables it\n", offPeak, d0, d1);
}

void rtTrapControl() {
    cd::test::RtTrapScope trap;
    void* p = ::operator new(16);
    ::operator delete(p);
    CHECK(trap.counts().allocations == 1 && trap.counts().deallocations == 1, "RtTrap positive control not trapped");
}

}  // namespace

int main() {
    manifestLaw();
    solverApi();
    feedbackAnalytic();
    driftSensitivity();
    creativeMotion();
    analogNoise();
    rtTrapControl();
    CHECK(g_rtCount == 0, "audio-path allocations/deallocations: %ld", g_rtCount);
    std::printf("UX-09 RtTrap over every feedback/creative audio call: %ld allocations+deallocations; positive control trapped\n", g_rtCount);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("runtime_thermal_feedback: all checks passed\n");
    return 0;
}
