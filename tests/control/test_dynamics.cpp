// T15 (transient detector, control level), T16 (sidechain load, control level), plus processor-level
// determinism, 0-frame behaviour and creative-drift properties. Synthetic fixtures only.
#include <algorithm>
#include <cmath>
#include <vector>
#include "TestHarness.h"
#include "control/ControlProcessor.h"
#include "control/CreativeDrift.h"
#include "control/SidechainLoad.h"
#include "control/TransientDetector.h"

using namespace cd;
using namespace cd::control;

constexpr double kFs = 48000.0;

// SYNTHETIC kick: 200 ms silence, then 60 Hz sine with 50 ms exponential decay at 0.8 amplitude.
static std::vector<double> kick(std::size_t total) {
    std::vector<double> x(total, 0.0);
    const std::size_t on = std::size_t(0.2 * kFs);
    for (std::size_t i = on; i < total; ++i) {
        const double t = double(i - on) / kFs;
        x[i] = 0.8 * std::exp(-t / 0.05) * std::sin(2 * std::numbers::pi * 60.0 * t);
    }
    return x;
}

static void transientT15() {
    const auto x = kick(std::size_t(0.6 * kFs));
    std::vector<double> P(x.size());
    TransientDetector td;
    td.prepare(kFs);
    td.process(x.data(), x.data(), int(x.size()), 0.0, P.data());
    double mx = 0;
    for (double v : P) mx = std::max(mx, std::fabs(v));
    CHECK(mx == 0.0, "amount 0 -> P identically 0 (max %g)", mx);

    td.reset();
    td.process(x.data(), x.data(), int(x.size()), 1.0, P.data());
    const std::size_t on = std::size_t(0.2 * kFs);
    double pre = 0, atOnset = 0;
    for (std::size_t i = 0; i < on; ++i) pre = std::max(pre, P[i]);
    for (std::size_t i = on; i < on + std::size_t(0.002 * kFs); ++i) atOnset = std::max(atOnset, P[i]);
    CHECK(pre == 0.0, "no preservation on silence (gate)");
    CHECK(atOnset > 0.5, "kick onset produces P>0: %g", atOnset);
    for (double v : P) CHECK(v >= 0.0 && v <= 1.0, "P in [0,1]");
    // Hold then release: P still high 4 ms after the peak, and decays well below by 100 ms.
    double peakAt = 0;
    std::size_t pk = on;
    for (std::size_t i = on; i < on + 2400; ++i) if (P[i] > peakAt) { peakAt = P[i]; pk = i; }
    CHECK(P[pk + std::size_t(0.004 * kFs)] > 0.9 * peakAt, "5 ms hold");
    CHECK(P[pk + std::size_t(0.1 * kFs)] < 0.1 * peakAt + 1e-3, "released by 100 ms: %g", P[pk + std::size_t(0.1 * kFs)]);
    // amount scales P linearly.
    std::vector<double> P5(x.size());
    td.reset();
    td.process(x.data(), x.data(), int(x.size()), 0.5, P5.data());
    double dmax = 0;
    for (std::size_t i = 0; i < P.size(); ++i) dmax = std::max(dmax, std::fabs(P5[i] - 0.5 * P[i]));
    CHECK(dmax < 1e-12, "P = amount*b: %g", dmax);
    // Steady tone: transient evidence fades (slow envelope catches up).
    const auto s = th::sine(200, 0.5, kFs, std::size_t(1.0 * kFs));
    std::vector<double> Ps(s.size());
    td.reset();
    td.process(s.data(), nullptr, int(s.size()), 1.0, Ps.data());
    CHECK(Ps.back() < 0.05, "steady tone P -> 0: %g", Ps.back());
    // Block-partition invariance.
    std::vector<double> Pp(x.size());
    td.reset();
    for (std::size_t i = 0; i < x.size();) {
        const std::size_t n = std::min<std::size_t>(37, x.size() - i);
        td.process(x.data() + i, x.data() + i, int(n), 1.0, Pp.data() + i);
        i += n;
    }
    double dd = 0;
    for (std::size_t i = 0; i < P.size(); ++i) dd = std::max(dd, std::fabs(P[i] - Pp[i]));
    CHECK(dd == 0.0, "partition invariant: %g", dd);
    // Below -60 dBFS gate: a quiet click gives nothing.
    std::vector<double> q(4800, 0.0), Pq(4800);
    q[100] = 5e-4;
    td.reset();
    td.process(q.data(), nullptr, 4800, 1.0, Pq.data());
    CHECK(*std::max_element(Pq.begin(), Pq.end()) == 0.0, "gated below -60 dBFS");
}

static double meanTail(const std::vector<double>& v, double frac) {
    const std::size_t n = std::size_t(double(v.size()) * frac);
    double s = 0;
    for (std::size_t i = v.size() - n; i < v.size(); ++i) s += v[i];
    return s / double(n);
}

static void sidechainT16() {
    constexpr double budget = 0.01;  // [A], test value; real value comes from the model manifest
    const double rms = std::pow(10.0, -18.0 / 20.0);
    const std::size_t n = std::size_t(1.0 * kFs);
    std::vector<double> out(n);
    for (double f : {500.0, 1000.0, 4000.0}) {
        const auto s = th::sine(f, rms * std::sqrt(2.0), kFs, n);
        SidechainLoad sc;
        sc.prepare(kFs);
        sc.setBudgetAmps(budget);
        CHECK(sc.process(SidechainSource::External, 1.0, 40.0, s.data(), s.data(), true, int(n), out.data()), "external used");
        const double mean = meanTail(out, 0.5);
        CHECK(std::fabs(mean - budget) / budget <= 0.01, "calibrated -18 dBFS sine @%g Hz: %.6g A vs %g", f, mean, budget);
        sc.reset();
        sc.process(SidechainSource::Blend, 2.0, 40.0, s.data(), nullptr, true, int(n), out.data());
        CHECK(std::fabs(meanTail(out, 0.5) - 2 * budget) / (2 * budget) <= 0.01, "depth 2 doubles (mono link)");
    }
    const auto loud = th::sine(1000.0, std::sqrt(2.0), kFs, n);  // 0 dBFS RMS: clamp at 4x
    SidechainLoad sc;
    sc.prepare(kFs);
    sc.setBudgetAmps(budget);
    sc.process(SidechainSource::External, 1.0, 40.0, loud.data(), loud.data(), true, int(n), out.data());
    CHECK(std::fabs(meanTail(out, 0.5) - 4 * budget) / (4 * budget) <= 0.01, "E/E_ref clamps at 4");
    double mx = *std::max_element(out.begin(), out.end());
    CHECK(mx <= 4 * budget * 1.0000001, "never above clamp");
    // Depth 0: identity <= 1e-7.
    sc.reset();
    sc.process(SidechainSource::External, 0.0, 40.0, loud.data(), loud.data(), true, int(n), out.data());
    mx = 0;
    for (double v : out) mx = std::max(mx, std::fabs(v));
    CHECK(mx <= 1e-7, "depth 0 -> I_ext == 0: %g", mx);
    // Silence on the external bus: zero current.
    std::vector<double> z(n, 0.0);
    sc.reset();
    sc.process(SidechainSource::External, 1.0, 40.0, z.data(), z.data(), true, int(n), out.data());
    CHECK(*std::max_element(out.begin(), out.end()) <= 1e-12, "external silence -> 0");
    // Disconnected bus: zero + not used.
    CHECK(!sc.process(SidechainSource::External, 1.0, 40.0, nullptr, nullptr, false, int(n), out.data()) &&
              *std::max_element(out.begin(), out.end()) == 0.0, "disconnected -> zero extra current");
    // Main source never uses the external bus.
    CHECK(!sc.process(SidechainSource::Main, 1.0, 40.0, loud.data(), loud.data(), true, int(n), out.data()) &&
              *std::max_element(out.begin(), out.end()) == 0.0, "Main source -> no external current");
    // HPF: a 20 Hz tone is strongly attenuated at 300 Hz corner, passes at 20 Hz corner far less so.
    const auto lf = th::sine(30.0, rms * std::sqrt(2.0), kFs, n);
    sc.reset();
    sc.process(SidechainSource::External, 1.0, 300.0, lf.data(), lf.data(), true, int(n), out.data());
    const double hi = meanTail(out, 0.25);
    sc.reset();
    sc.process(SidechainSource::External, 1.0, 20.0, lf.data(), lf.data(), true, int(n), out.data());
    const double lo = meanTail(out, 0.25);
    CHECK(hi < 0.05 * budget && lo > 0.4 * budget, "sidechain HPF: 300 Hz corner %g vs 20 Hz corner %g", hi, lo);
}

static void processorSidechain() {
    ControlProcessor cp;
    cp.prepare(kFs, 4800, 11);
    cp.setSidechainBudgetA(0.02);
    core::ParamValues p = core::defaultValues();
    p[core::indexOf("sidechain.source")] = 1;  // External
    p[core::indexOf("sidechain.depth")] = 1.0;
    const auto ext = th::sine(1000.0, std::pow(10.0, -18.0 / 20.0) * std::sqrt(2.0), kFs, 4800);
    const std::vector<double> mainSig(4800, 0.25);
    const double* mch[1] = {mainSig.data()};
    const double* sch[1] = {ext.data()};
    core::ModulationBlock mb;
    ControlContext ctx;
    ctx.frames = 4800;
    // Disconnected: zero extra current + status.
    cp.processBlock(p, ctx, AudioView{mch, 1}, nullptr, mb);
    CHECK(cp.status().sidechainMissing && !cp.status().sidechainConnected, "status: external bus missing");
    CHECK(*std::max_element(mb.externalLoadA, mb.externalLoadA + 4800) == 0.0 && mb.slot[0].sidechainMode == 1, "no current when disconnected");
    AudioView scv{sch, 1};
    for (int i = 0; i < 12; ++i) cp.processBlock(p, ctx, AudioView{mch, 1}, &scv, mb);
    CHECK(cp.status().sidechainConnected && !cp.status().sidechainMissing, "connected");
    CHECK(std::fabs(mb.externalLoadA[4799] - 0.02) / 0.02 < 0.02, "processor current %g", mb.externalLoadA[4799]);
    // Sidechain input is const and never copied to anything audio-facing (ModulationBlock has no audio).
    CHECK(ext[10] == th::sine(1000.0, std::pow(10.0, -18.0 / 20.0) * std::sqrt(2.0), kFs, 11)[10], "input untouched");
    p[core::indexOf("sidechain.source")] = 0;
    cp.processBlock(p, ctx, AudioView{mch, 1}, &scv, mb);
    CHECK(*std::max_element(mb.externalLoadA, mb.externalLoadA + 4800) == 0.0 && !cp.status().sidechainMissing, "Main -> none");
    // Transient buffer through the processor: amount 0 -> all zero; kick -> positive.
    const auto k = kick(std::size_t(0.4 * kFs) & ~std::size_t(0));
    ControlProcessor c2;
    c2.prepare(kFs, int(k.size()), 1);
    const double* kc[2] = {k.data(), k.data()};
    ctx.frames = int(k.size());
    core::ParamValues q = core::defaultValues();
    c2.processBlock(q, ctx, AudioView{kc, 2}, nullptr, mb);
    CHECK(*std::max_element(mb.transientPreserve, mb.transientPreserve + k.size()) == 0.0, "processor amount 0 -> P==0");
    c2.reset();
    q[core::indexOf("transient.preserve")] = 1.0;
    c2.processBlock(q, ctx, AudioView{kc, 2}, nullptr, mb);
    CHECK(*std::max_element(mb.transientPreserve, mb.transientPreserve + k.size()) > 0.5, "processor kick -> P>0");
}

static void determinismAndZeroFrames() {
    auto makeRun = [](uint64_t seed, int part) {
        ControlProcessor cp;
        cp.prepare(kFs, 8192, seed);
        core::ParamValues p = core::defaultValues();
        p[core::indexOf("macro.drift")] = 1.0;
        p[core::indexOf("pitchDrift.enabled")] = 1;
        core::ModulationBlock mb;
        std::vector<double> trace;
        ControlContext ctx;
        for (int done = 0; done < 96000; done += part) {
            ctx.frames = part;
            cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
        }
        trace.push_back(mb.slot[0].creativeCutoffOct);
        trace.push_back(mb.slot[1].creativeCutoffOct);
        trace.push_back(mb.slot[0].creativeDelayMs);
        trace.push_back(mb.slot[1].creativeDelayMs);
        return trace;
    };
    const auto a = makeRun(42, 480), b = makeRun(42, 480), c = makeRun(43, 480), d = makeRun(42, 8000);
    CHECK(a == b, "same seed + partition -> identical");
    CHECK(a != c, "different seed -> different motion");
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(std::fabs(a[i] - d[i]) < 1e-12, "partition independent tick clock [%zu]", i);
    CHECK(a[0] != a[1], "slots use independent streams");

    ControlProcessor cp;
    cp.prepare(kFs, 512, 5);
    core::ParamValues p = core::defaultValues();
    p[core::indexOf("macro.drift")] = 1.0;
    p[core::indexOf("pitchDrift.enabled")] = 1;
    p[core::indexOf("tempo.enabled")] = 1;
    p[core::indexOf("tempo.depth")] = 1.0;
    core::ModulationBlock m1, m2, m3;
    ControlContext ctx;
    ctx.frames = 512;
    ctx.bpm = 100.0;
    for (int i = 0; i < 50; ++i) cp.processBlock(p, ctx, AudioView{}, nullptr, m1);
    const uint64_t ticks = cp.drift(0).ticks();
    const double phase = cp.status().tempoPhase;
    ctx.frames = 0;
    cp.processBlock(p, ctx, AudioView{}, nullptr, m2);
    cp.processBlock(p, ctx, AudioView{}, nullptr, m3);
    CHECK(cp.drift(0).ticks() == ticks && cp.status().tempoPhase == phase, "0-frame call advances no clock");
    CHECK(m2.slot[0].creativeCutoffOct == m3.slot[0].creativeCutoffOct && m2.slot[0].creativeCutoffOct == m1.slot[0].creativeCutoffOct &&
              m2.slot[0].creativeDelayMs == m1.slot[0].creativeDelayMs, "0-frame outputs hold last block's creative motion");
    // Macros still reflect parameters on a 0-frame call.
    p[core::indexOf("macro.age")] = 1.0;
    cp.processBlock(p, ctx, AudioView{}, nullptr, m2);
    CHECK(std::fabs(m2.slot[0].ageYearsMin - 60.0) < 1e-9, "macros evaluated on 0-frame call");
    // Frames beyond maxBlock are clamped and flagged, not overrun.
    ctx.frames = 100000;
    cp.processBlock(p, ctx, AudioView{}, nullptr, m2);
    CHECK(cp.status().blockClamped, "oversized block flagged");
    // Zero-latency mode disables the delay modulation.
    p[core::indexOf("quality.zeroLatency")] = 1;
    ctx.frames = 512;
    cp.processBlock(p, ctx, AudioView{}, nullptr, m2);
    CHECK(!m2.slot[0].creativeDelayActive && m2.slot[0].creativeDelayMs == 0.0, "zero latency disables pitch drift");
}

static void driftProperties() {
    CreativeDrift cd;
    cd.prepare(kFs, 2024, 0);
    double sum = 0, sum2 = 0, maxQ = 0, prevDelay = cd.delayMs(1.0), maxStep = 0;
    long cnt = 0;
    for (int blk = 0; blk < 200000; ++blk) {  // 100 Hz ticks: 480-sample blocks == one tick each
        cd.advance(480);
        const double q = cd.q();
        sum += q; sum2 += q * q; maxQ = std::max(maxQ, std::fabs(q));
        maxStep = std::max(maxStep, std::fabs(cd.delayMs(1.0) - prevDelay));
        prevDelay = cd.delayMs(1.0);
        ++cnt;
    }
    const double mean = sum / cnt, sd = std::sqrt(sum2 / cnt - mean * mean);
    CHECK(maxQ < 1.0, "q bounded by tanh");
    CHECK(std::fabs(mean) < 0.15 && sd > 0.3 && sd < 0.7, "stationary stats: mean %g sd %g", mean, sd);
    CHECK(maxStep <= 0.002 / 1.0 * 1000.0 / 100.0 + 1e-12, "slew limit 2 ms/s: step %g ms per 10 ms", maxStep);
    CHECK(cd.cutoffOct(0.0) == 0.0 && cd.delayMs(0.0) == 0.0, "d=0 removes creative motion");
    CHECK(std::fabs(cd.cutoffOct(1.0)) <= 0.15 && std::fabs(cd.delayMs(1.0)) <= 1.5, "bounds at d=1");
    // Checkpoint: restore reproduces the future exactly.
    CreativeDrift a, b;
    a.prepare(kFs, 9, 1);
    b.prepare(kFs, 123456, 0);
    a.advance(12345);
    const auto st = a.save();
    b.restore(st);
    for (int i = 0; i < 1000; ++i) {
        a.advance(777);
        b.advance(777);
        CHECK(a.q() == b.q() && a.delayMs(0.8) == b.delayMs(0.8), "checkpoint replay %d", i);
    }
}

int main() {
    transientT15();
    sidechainT16();
    processorSidechain();
    determinismAndZeroFrames();
    driftProperties();
    if (g_fail) { std::printf("dynamics: %d failure(s)\n", g_fail); return 1; }
    std::printf("dynamics: all passed\n");
    return 0;
}
