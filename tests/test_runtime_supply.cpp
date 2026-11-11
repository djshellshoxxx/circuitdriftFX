// CircuitDrift tests: reduced supply reservoir + Sag binding, T15 transient preservation and T16
// external sidechain load at ENGINE level (resume packet 3; 01-runtime "Supply reservoirs",
// 02-musical "Transient recovery and external sidechain").
// Every circuit and supply number is a labelled SYNTHETIC fixture (fixture-v1 diode clipper +
// runtime::syntheticFactorySupply()); the kick and sine signals are synthetic test fixtures.
// Nothing here validates hardware (G01 open). Owner: engine2 (runtime).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <span>
#include <string>
#include <vector>
#include "control/Dynamics.h"
#include "control/Macros.h"
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

long g_rtCount = 0;   // allocations + deallocations observed inside audio calls

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
};
using Buf = std::vector<std::vector<double>>;
constexpr double kFs = 48000;

// SYNTHETIC kick fixture: silence, then a pitch-swept (50 Hz + 70 Hz e^{-t/30 ms}) sine with a
// 250 ms decay; amplitude `amp` (host full scale 1.0).
Buf kick(std::size_t n, double onsetSec, double amp, int channels = 1) {
    Buf x(std::size_t(channels), std::vector<double>(n, 0.0));
    double ph = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = double(i) / kFs - onsetSec;
        if (t < 0) continue;
        const double f = 50.0 + 70.0 * std::exp(-t / 0.030);
        ph += 2.0 * M_PI * f / kFs;
        for (auto& c : x) c[i] = amp * std::exp(-t / 0.250) * std::sin(ph);
    }
    return x;
}

Buf sine(std::size_t n, double hz, double amp, int channels = 1) {
    Buf x(std::size_t(channels), std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i)
        for (auto& c : x) c[i] = amp * std::sin(2.0 * M_PI * hz * double(i) / kFs);
    return x;
}

Buf noise(std::size_t n, uint64_t seed, double amp, int channels = 1) {
    Buf x(std::size_t(channels), std::vector<double>(n, 0.0));
    Lcg g{seed};
    for (std::size_t i = 0; i < n; ++i)
        for (auto& c : x) c[i] = amp * g.d();
    return x;
}

double peakDiff(const Buf& a, const Buf& b, std::size_t from = 0) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = from; i < a[c].size(); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}
double rms(const std::vector<double>& v, std::size_t a, std::size_t b) {
    double s = 0;
    for (std::size_t i = a; i < b; ++i) s += v[i] * v[i];
    return std::sqrt(s / double(b - a));
}
bool finite(const Buf& a) { for (const auto& c : a) for (double v : c) if (!std::isfinite(v)) return false; return true; }

EngineConfig supplyConfig(int channels = 1) {
    EngineConfig c;
    c.sampleRate = kFs; c.maxBlock = 512; c.channels = channels;
    c.quality.mode = QualityMode::Studio;
    c.circuit = syntheticDiodeClipper();
    c.circuit.supply = syntheticFactorySupply();
    c.initial.hpfEnabled = false; c.initial.softClipEnabled = false; c.initial.autoGain = false;
    return c;
}

// Ideal (stiff) rail with the same nominal volts: reference for "sag loss".
EngineConfig stiffConfig(int channels = 1) {
    EngineConfig c = supplyConfig(channels);
    c.circuit.supply.sourceOhm = 1e-6;
    c.circuit.supply.sourceV = 1.0 + 1e-6 * c.circuit.supply.idleA;   // V_nom = 1.0 V like the factory supply
    return c;
}

struct Render {
    Buf out;
    std::vector<double> rail;   // rail fraction r per host sample (channel 0)
    uint32_t status = 0;
};

// Renders through `eng` with calls cycling through `parts`; optional sidechain bus and events
// (absolute sample times). Every audio call runs under the RtTrap.
struct Ev { std::size_t t; uint32_t id; double norm; };
Render render(Engine& eng, const Buf& in, const Buf* side, const std::vector<uint32_t>& parts, const std::vector<Ev>& evs = {}) {
    const int C = int(in.size());
    const std::size_t N = in[0].size();
    Render r;
    r.out.assign(std::size_t(C), std::vector<double>(N, 0.0));
    r.rail.assign(N, 0.0);
    std::vector<TransactionEvent> be;
    be.reserve(64);
    std::size_t pos = 0, k = 0, ev = 0;
    while (pos < N) {
        const uint32_t f = uint32_t(std::min<std::size_t>(parts[k++ % parts.size()], N - pos));
        be.clear();
        while (ev < evs.size() && evs[ev].t < pos + f) { be.push_back(TransactionEvent::param(evs[ev].id, evs[ev].norm, uint32_t(evs[ev].t - pos))); ++ev; }
        const double* ip[2] = {in[0].data() + pos, C > 1 ? in[1].data() + pos : nullptr};
        double* op[2] = {r.out[0].data() + pos, C > 1 ? r.out[1].data() + pos : nullptr};
        const double* sp[2] = {};
        AudioBuses b{C, ip, op};
        if (side) {
            sp[0] = (*side)[0].data() + pos;
            sp[1] = side->size() > 1 ? (*side)[1].data() + pos : nullptr;
            b.sideChannels = int(side->size()); b.side = sp;
        }
        {
            cd::test::RtTrapScope trap;
            const ProcessResult res = eng.process(ProcessContext{kFs, int64_t(pos), 0, f, false}, b,
                                                  std::span<const TransactionEvent>(be.data(), be.size()));
            r.status |= res.status;
            g_rtCount += trap.counts().allocations + trap.counts().deallocations;
        }
        // Rail trace of the call (calls here never exceed maxBlock unless trace is not needed).
        if (f <= 512 && f > 0) {
            const double* tr = eng.active()->circuit().railFractionTrace(0);
            for (uint32_t i = 0; i < f; ++i) r.rail[pos + i] = tr[i];
        }
        pos += f;
    }
    return r;
}

Engine& prepared(Engine& e, const EngineConfig& c) {
    std::string err;
    CHECK(e.prepare(c, err), "prepare: %s", err.c_str());
    return e;
}

// ---------------------------------------------------------------------------------------------
void supplyBasics() {
    std::string e;
    // Analytic idle/static checks of the reduced model.
    const SyntheticSupply s = syntheticFactorySupply();
    CHECK(std::fabs(s.nominalV() - (1.1 - 100 * 1e-3)) <= 1e-15, "nominal rail");
    Engine eng;
    prepared(eng, supplyConfig());
    const Buf z(1, std::vector<double>(4800, 0.0));
    const Render rz = render(eng, z, nullptr, {480});
    const auto d = eng.diagnostics();
    CHECK(d.circuit.supplyEnabled && std::fabs(d.circuit.railFraction[0] - 1.0) <= 1e-12 && peakDiff(rz.out, z) == 0.0,
          "idle rail %.15f / silent output", d.circuit.railFraction[0]);
    CHECK(d.effective.supportsSag && std::fabs(d.effective.reservoirC - s.cBaseF) <= 1e-18, "Sag binding derived from supply manifest");
    // Static load: a DC-like steady draw settles to V_src - R_src*(I_idle + I_dyn) (analytic).
    {
        Engine st;
        EngineConfig c = supplyConfig();
        c.initialMacros.sag = 0.0;
        prepared(st, c);
        Buf dc(1, std::vector<double>(48000, 0.1));   // 0.1 V DC through the clipper (small-signal: x ~ 0.1 V)
        render(st, dc, nullptr, {512});
        const double x = st.active()->circuit().voltage(0, 2);
        const double expect = s.sourceV - s.sourceOhm * (s.idleA + std::fabs(x) / s.loadOhm);
        const double v = st.diagnostics().circuit.railVolts[0];
        CHECK(std::fabs(v - expect) <= 1e-9, "static rail %.12f vs analytic %.12f", v, expect);
        std::printf("Supply (SYNTHETIC): V_nom %.3f V; static 0.1 V input rail %.9f V vs analytic %.9f V (|err| %.2e)\n",
                    s.nominalV(), v, expect, std::fabs(v - expect));
    }
    // Sag macro: C_eff = max(C_min, C_base/(1+3s)); at s=1 the reservoir drops faster and further.
    Engine e0, e1;
    EngineConfig c0 = supplyConfig(), c1 = supplyConfig();
    c0.initialMacros.sag = 0; c1.initialMacros.sag = 1;
    prepared(e0, c0); prepared(e1, c1);
    const Buf k = kick(9600, 0.02, 0.9);
    const Render r0 = render(e0, k, nullptr, {480}), r1 = render(e1, k, nullptr, {480});
    const double ce = e1.diagnostics().effective.reservoirC;
    CHECK(std::fabs(ce - std::max(s.cMinF, s.cBaseF / 4.0)) <= 1e-18 && std::fabs(e1.diagnostics().circuit.reservoirF - ce) <= 1e-18,
          "C_eff at s=1 %.6g", ce);
    const double min0 = *std::min_element(r0.rail.begin(), r0.rail.end()), min1 = *std::min_element(r1.rail.begin(), r1.rail.end());
    CHECK(min1 < min0 && min1 >= s.minRailFraction - 1e-12 && finite(r1.out), "sag deeper at s=1: %.4f vs %.4f", min1, min0);
    std::printf("Sag binding: C_eff(s=1) %.1f uF (C_base %.0f uF); kick rail minimum s=0 %.4f, s=1 %.4f (bound r_min %.2f)\n",
                ce * 1e6, s.cBaseF * 1e6, min0, min1, s.minRailFraction);
    // Sad paths: invalid supply manifests rejected at prepare.
    for (int bad = 0; bad < 6; ++bad) {
        EngineConfig b = supplyConfig();
        switch (bad) {
            case 0: b.circuit.supply.cMinF = 2 * b.circuit.supply.cBaseF; break;
            case 1: b.circuit.supply.sourceOhm = -1; break;
            case 2: b.circuit.supply.idleA = 1.0; break;                     // V_nom <= 0
            case 3: b.circuit.supply.minRailFraction = 0; break;
            case 4: b.circuit.supply.headroomV = std::nan(""); break;
            case 5: b.circuit.supply.iBudgetA = -1e-3; break;
        }
        Engine x;
        CHECK(!x.prepare(b, e) && !x.prepared(), "invalid supply %d accepted", bad);
    }
    // Non-finite controls into CircuitRuntime are clamped/neutral (no NaN propagation).
    {
        CircuitConfig cc = syntheticDiodeClipper();
        cc.supply = syntheticFactorySupply();
        cc.os.factor = 2;
        CircuitRuntime cr;
        CHECK(cr.prepare(cc, e), "circuit prepare %s", e.c_str());
        std::vector<double> x(64, 0.5), w(64), dr(64), bad(64, std::nan("")), inf(64, INFINITY), neg(64, -5.0);
        const double* in[1] = {x.data()};
        double* wp[1] = {w.data()};
        double* dp[1] = {dr.data()};
        CircuitControls ctl;
        ctl.reservoirC = bad.data(); ctl.sag = inf.data(); ctl.preserve = bad.data(); ctl.extCurrent = neg.data();
        ctl.tempSensitivity = bad.data(); ctl.source = LoadSource::Blend;
        cd::test::RtTrapScope trap;
        cr.process(in, wp, dp, 64, ctl);
        g_rtCount += trap.counts().allocations + trap.counts().deallocations;
        bool ok = true;
        for (double v : w) ok = ok && std::isfinite(v);
        const auto dd = cr.diagnostics();
        CHECK(ok && std::isfinite(dd.railVolts[0]) && dd.reservoirF == cc.supply.cBaseF, "non-finite controls propagated");
    }
    std::printf("Supply sad paths: 6 invalid manifests rejected; NaN/Inf/negative per-sample controls clamped (finite output)\n");
}

// Partition invariance of the full supply + detector + sidechain path.
void partitions() {
    const Buf in = kick(24000, 0.05, 0.9, 2), side = noise(24000, 7, 0.5, 2);
    std::vector<Ev> ev = {{3000, 1001, 1.0}, {9000, 1025, 0.7}, {12000, 1022, 0.75}, {12001, 1023, 0.5}};
    Render ref;
    double worst = 0;
    bool first = true;
    for (uint32_t p : {1u, 17u, 64u, 127u, 512u, 8192u}) {
        Engine eng;
        EngineConfig c = supplyConfig(2);
        prepared(eng, c);
        const Render r = render(eng, in, &side, {p}, ev);
        if (first) { ref = r; first = false; continue; }
        worst = std::max(worst, peakDiff(r.out, ref.out));
    }
    CHECK(worst == 0.0, "supply/detector path not partition invariant: %.3e", worst);
    std::printf("Partition invariance {1,17,64,127,512,8192} (stereo kick, Sag/preserve/Blend sidechain automation): max diff %.1e\n", worst);
}

// T15: transient preservation.
void t15() {
    std::string e;
    const std::size_t N = 24000;
    const double onset = 0.05;
    const Buf in = kick(N, onset, 0.9);
    Engine stiff, unprot, prot, zero;
    EngineConfig cs = stiffConfig(), cu = supplyConfig(), cp = supplyConfig();
    cs.initialMacros.sag = cu.initialMacros.sag = cp.initialMacros.sag = 1.0;
    cp.dynamics.transientPreserve = 1.0;
    prepared(stiff, cs); prepared(unprot, cu); prepared(prot, cp); prepared(zero, cu);
    const Render rs = render(stiff, in, nullptr, {480}), ru = render(unprot, in, nullptr, {480}), rp = render(prot, in, nullptr, {480});
    const int L = unprot.latencySamples();
    CHECK(L == stiff.latencySamples() && L == prot.latencySamples(), "latency");
    const std::size_t a = std::size_t(onset * kFs) + std::size_t(L), b = a + std::size_t(0.010 * kFs);
    const double es = rms(rs.out[0], a, b), eu = rms(ru.out[0], a, b), ep = rms(rp.out[0], a, b);
    const double lossU = 20 * std::log10(es / eu), lossP = 20 * std::log10(es / ep);
    const double reduction = 1.0 - lossP / lossU;
    CHECK(lossU >= 3.0, "T15 unprotected first-10 ms sag loss %.3f dB < 3 dB", lossU);
    CHECK(reduction >= 0.5, "T15 full preservation reduces sag loss by %.1f%% (< 50%%): %.3f -> %.3f dB", 100 * reduction, lossU, lossP);
    // Physical reservoir trajectory unchanged by preservation (bit-identical).
    double railDiff = 0;
    for (std::size_t i = 0; i < N; ++i) railDiff = std::max(railDiff, std::fabs(rp.rail[i] - ru.rail[i]));
    CHECK(railDiff == 0.0, "T15 preservation changed the reservoir trajectory: %.3e", railDiff);
    // Effective r rises above physical r while preserving.
    const double reff = prot.diagnostics().circuit.effectiveRailFraction[0], rphys = prot.diagnostics().circuit.railFraction[0];
    CHECK(reff >= rphys, "r_eff %.6f < r %.6f", reff, rphys);
    // amount 0 identity (a): constant amount 0 vs a no-preservation CircuitRuntime path (P = null).
    {
        CircuitConfig cc = syntheticDiodeClipper();
        cc.supply = syntheticFactorySupply();
        cc.os.factor = 4;
        CircuitRuntime c1, c2;
        CHECK(c1.prepare(cc, e) && c2.prepare(cc, e), "prepare");
        control::TransientDetector td;
        td.prepare(kFs);
        std::vector<double> x(in[0]), w1(N), w2(N), d1(N), d2(N), pres(N), cE(N, cc.supply.cBaseF / 4.0), sg(N, 1.0);
        for (std::size_t i = 0; i < N; ++i) pres[i] = control::TransientDetector::strength(0.0, td.process(x[i], x[i]));
        double maxDiff = 0;
        for (std::size_t pos = 0; pos < N; pos += 480) {
            const double* ip[1] = {x.data() + pos};
            double* wa[1] = {w1.data() + pos}; double* da[1] = {d1.data() + pos};
            double* wb[1] = {w2.data() + pos}; double* db[1] = {d2.data() + pos};
            CircuitControls k1, k2;
            k1.reservoirC = k2.reservoirC = cE.data() + pos; k1.sag = k2.sag = sg.data() + pos;
            k2.preserve = pres.data() + pos;
            c1.process(ip, wa, da, 480, k1);
            c2.process(ip, wb, db, 480, k2);
        }
        for (std::size_t i = 0; i < N; ++i) maxDiff = std::max(maxDiff, std::fabs(w1[i] - w2[i]));
        CHECK(maxDiff <= 1e-7, "T15 amount 0 identity (circuit): %.3e", maxDiff);
        std::printf("T15 amount 0 vs no-preservation circuit path: peak %.1e (limit 1e-7)\n", maxDiff);
    }
    // amount 0 identity (b): an engine automated 0 -> 1 -> 0 returns to the amount-0 output once the
    // 20 ms smoothing has settled (the physical state never depended on P).
    {
        const Buf in2 = kick(48000, onset, 0.9);
        const Render rz = render(zero, in2, nullptr, {480});
        Engine aut;
        prepared(aut, cu);
        const Render ra = render(aut, in2, nullptr, {480}, {{0, 1025, 1.0}, {2400 + std::size_t(onset * kFs), 1025, 0.0}});
        const std::size_t settle = 2400 + std::size_t(onset * kFs) + 24000;   // +500 ms (25 smoothing time constants)
        const double d = peakDiff(ra.out, rz.out, settle);
        const double during = peakDiff(ra.out, rz.out);
        CHECK(d <= 1e-7 && during > 1e-3, "T15 amount 0 identity after automation: %.3e (during %.3e)", d, during);
        std::printf("T15 engine amount 0 identity: automated 0->1->0 vs constant 0 500 ms after returning to 0: peak %.1e (limit 1e-7); while "
                    "preserving %.3e\n", d, during);
    }
    std::printf("T15 (SYNTHETIC kick, Sag 1, stiff-rail reference): first-10 ms sag loss unprotected %.3f dB, full preservation %.3f dB "
                "-> reduction %.1f%% (limits >=3 dB, >=50%%); reservoir trajectory diff %.1e; final r %.4f r_eff %.4f\n",
                lossU, lossP, 100 * reduction, railDiff, rphys, reff);
}

// T16: external sidechain load.
void t16() {
    const std::size_t N = 24000;
    const Buf main = sine(N, 440, 0.3), quiet(1, std::vector<double>(N, 0.0));
    const Buf sideNoise = noise(N, 99, 0.6, 2);
    // depth 0 identity: External/Blend at depth 0 with a loud bus vs no bus at all.
    double idWorst = 0;
    for (LoadSource src : {LoadSource::External, LoadSource::Blend}) {
        Engine a, b;
        EngineConfig c = supplyConfig();
        c.initialMacros.sag = 0.5;
        c.dynamics.sidechainSource = src; c.dynamics.sidechainDepth = 0.0;
        prepared(a, c); prepared(b, c);
        const Render ra = render(a, main, &sideNoise, {480}), rb = render(b, main, nullptr, {480});
        idWorst = std::max(idWorst, peakDiff(ra.out, rb.out));
    }
    CHECK(idWorst <= 1e-7, "T16 depth 0 identity %.3e", idWorst);
    // Disconnected bus: zero extra current + status; identical to depth 0.
    Engine dis, d0;
    EngineConfig cd = supplyConfig(), cz = supplyConfig();
    cd.dynamics.sidechainSource = cz.dynamics.sidechainSource = LoadSource::External;
    cd.dynamics.sidechainDepth = 1.0; cz.dynamics.sidechainDepth = 0.0;
    prepared(dis, cd); prepared(d0, cz);
    const Render rdis = render(dis, main, nullptr, {480}), rd0 = render(d0, main, nullptr, {480});
    CHECK(dis.diagnostics().extCurrentA == 0.0 && (rdis.status & status::kSidechainMissing) && !dis.diagnostics().sidechainConnected &&
              peakDiff(rdis.out, rd0.out) == 0.0,
          "T16 disconnected: I_ext %.3e status %x diff %.3e", dis.diagnostics().extCurrentA, rdis.status, peakDiff(rdis.out, rd0.out));
    // External silence: connected, zero current, no missing status, identical output.
    {
        Engine es;
        prepared(es, cd);
        const Buf silent(2, std::vector<double>(N, 0.0));
        const Render r = render(es, main, &silent, {480});
        CHECK(es.diagnostics().extCurrentA == 0.0 && !(r.status & status::kSidechainMissing) && peakDiff(r.out, rd0.out) == 0.0,
              "T16 external silence");
    }
    // Calibrated -18 dBFS RMS sine on the bus: I_ext = depth * I_budget within 1%.
    double worstErr = 0, meas[2] = {};
    for (int k = 0; k < 2; ++k) {
        const double depth = k == 0 ? 1.0 : 2.0;
        Engine ec;
        EngineConfig c = supplyConfig();
        c.dynamics.sidechainSource = LoadSource::External; c.dynamics.sidechainDepth = depth;
        prepared(ec, c);
        const double amp = std::pow(10.0, -18.0 / 20.0) * std::sqrt(2.0);
        const Buf cal = sine(N, 1000, amp, 2);
        // Average the reported current over the last 100 ms (after settling) with 1-sample reads.
        double sum = 0;
        std::size_t cnt = 0;
        std::vector<uint32_t> parts = {480};
        const Render r = render(ec, main, &cal, parts);
        (void)r;
        // Re-run the tail sample by sample for the average (same engine continues with the same signal).
        const Buf tailMain = sine(4800, 440, 0.3), tailCal = sine(4800, 1000, amp, 2);
        for (std::size_t i = 0; i < 4800; ++i) {
            const double ip0 = tailMain[0][i];
            double o0 = 0;
            const double* ip[1] = {&ip0};
            double* op[1] = {&o0};
            const double* sp[2] = {&tailCal[0][i], &tailCal[1][i]};
            AudioBuses b{1, ip, op, 2, sp};
            cd::test::RtTrapScope trap;
            ec.process(ProcessContext{kFs, int64_t(N + i), 0, 1, false}, b, std::span<const TransactionEvent>());
            g_rtCount += trap.counts().allocations + trap.counts().deallocations;
            sum += ec.diagnostics().extCurrentA;
            ++cnt;
        }
        meas[k] = sum / double(cnt);
        const double expect = depth * syntheticFactorySupply().iBudgetA;
        worstErr = std::max(worstErr, std::fabs(meas[k] / expect - 1.0));
    }
    CHECK(worstErr <= 0.01, "T16 calibrated sine current error %.3f%%", 100 * worstErr);
    // No sidechain leak: silent main + loud bus -> exactly silent output; main sine + bus noise ->
    // output change uncorrelated with the bus audio and no bus energy in the output.
    double leakPeak = 0, corrDiff = 0, corrOut = 0, eFrac = 0;
    {
        Engine s1;
        EngineConfig c = supplyConfig();
        c.dynamics.sidechainSource = LoadSource::Blend; c.dynamics.sidechainDepth = 2.0; c.initialMacros.sag = 1.0;
        prepared(s1, c);
        const Render r = render(s1, quiet, &sideNoise, {480});
        for (double v : r.out[0]) leakPeak = std::max(leakPeak, std::fabs(v));
        Engine wi, wo;
        prepared(wi, c); prepared(wo, c);
        const Render ri = render(wi, main, &sideNoise, {480}), ro = render(wo, main, nullptr, {480});
        const int L = wi.latencySamples();
        // Correlate against the bus signal aligned at the declared latency (and unaligned, worst of).
        auto corr = [&](const std::vector<double>& y, int lag) {
            double sxy = 0, sxx = 0, syy = 0;
            for (std::size_t i = 4800; i + std::size_t(std::max(lag, 0)) < N; ++i) {
                const double x = sideNoise[0][i], v = y[i + std::size_t(std::max(lag, 0))];
                sxy += x * v; sxx += x * x; syy += v * v;
            }
            return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0.0;
        };
        std::vector<double> diff(N);
        for (std::size_t i = 0; i < N; ++i) diff[i] = ri.out[0][i] - ro.out[0][i];
        for (int lag : {0, L}) {
            corrDiff = std::max(corrDiff, std::fabs(corr(diff, lag)));
            corrOut = std::max(corrOut, std::fabs(corr(ri.out[0], lag)));
        }
        // Energy of the best least-squares projection of the bus audio onto the output, relative to the output.
        double sxy = 0, sxx = 0, syy = 0;
        for (std::size_t i = 4800; i + std::size_t(L) < N; ++i) {
            sxy += sideNoise[0][i] * ri.out[0][i + std::size_t(L)]; sxx += sideNoise[0][i] * sideNoise[0][i];
            syy += ri.out[0][i + std::size_t(L)] * ri.out[0][i + std::size_t(L)];
        }
        eFrac = (sxy * sxy / sxx) / syy;
        const double changeDb = 20 * std::log10(rms(ri.out[0], 12000, N) / rms(ro.out[0], 12000, N));
        CHECK(leakPeak == 0.0, "T16 sidechain leaked into silent main output: %.3e", leakPeak);
        CHECK(corrDiff < 0.05 && corrOut < 0.05 && eFrac < 1e-3, "T16 sidechain correlation diff %.4f out %.4f energy fraction %.2e",
              corrDiff, corrOut, eFrac);
        CHECK(changeDb < -0.1, "T16 Blend sidechain load had no audible sag effect (%.3f dB)", changeDb);
        std::printf("T16 no leak: silent main + loud bus -> output peak %.1e; main sine + bus noise: |corr(out-change, bus)| %.4f, "
                    "|corr(out, bus)| %.4f, projected bus energy fraction %.2e (limits 0.05/0.05/1e-3); Blend depth 2 level change %.2f dB\n",
                    leakPeak, corrDiff, corrOut, eFrac, changeDb);
    }
    // Kick on the bus (External): the reservoir sags with the bus kick and the main sine dips.
    {
        Engine ek, en;
        EngineConfig c = supplyConfig();
        c.dynamics.sidechainSource = LoadSource::External; c.dynamics.sidechainDepth = 2.0;
        prepared(ek, c); prepared(en, c);
        const Buf bk = kick(N, 0.2, 0.9, 2);
        const Render rk = render(ek, main, &bk, {480}), rn = render(en, main, nullptr, {480});
        const double minK = *std::min_element(rk.rail.begin() + 4800, rk.rail.end());
        const double minN = *std::min_element(rn.rail.begin() + 4800, rn.rail.end());
        const std::size_t a = std::size_t(0.2 * kFs), b = a + 4800;
        const double dipDb = 20 * std::log10(rms(rk.out[0], a, b) / rms(rn.out[0], a, b));
        CHECK(minK < minN - 0.02 && dipDb < -0.3, "T16 bus kick: rail min %.4f vs %.4f, dip %.3f dB", minK, minN, dipDb);
        std::printf("T16 (SYNTHETIC supply): depth 0 identity peak %.1e (limit 1e-7); disconnected -> I_ext 0 + kSidechainMissing, output "
                    "identical; -18 dBFS 1 kHz bus sine -> I_ext %.4f mA (depth 1), %.4f mA (depth 2), worst error %.3f%% (limit 1%%); "
                    "External bus kick: rail min %.4f vs %.4f, main level %.2f dB over 100 ms\n",
                    idWorst, meas[0] * 1e3, meas[1] * 1e3, 100 * worstErr, minK, minN, dipDb);
    }
    // Sad path: NaN on the bus is sanitized (detector) and the output stays finite.
    {
        Engine en;
        prepared(en, cd);
        Buf bad = sideNoise;
        for (std::size_t i = 0; i < N; i += 97) bad[0][i] = std::nan("");
        const Render r = render(en, main, &bad, {64});
        CHECK(finite(r.out) && std::isfinite(en.diagnostics().extCurrentA), "NaN sidechain propagated");
    }
}

// Selector precedence (Main / External / Blend) through the engine: composeLoadCurrent semantics.
void selector() {
    const std::size_t N = 12000;
    const Buf main = sine(N, 220, 0.6);
    const double amp = std::pow(10.0, -18.0 / 20.0) * std::sqrt(2.0);
    const Buf cal = sine(N, 1000, amp, 2);
    double minR[3] = {};
    for (int s = 0; s < 3; ++s) {
        Engine e;
        EngineConfig c = supplyConfig();
        c.initialMacros.sag = 1.0;
        c.dynamics.sidechainSource = LoadSource(s); c.dynamics.sidechainDepth = 1.0;
        prepared(e, c);
        const Render r = render(e, main, &cal, {480});
        minR[s] = *std::min_element(r.rail.begin() + 2400, r.rail.end());
        CHECK(e.diagnostics().sidechainSource == LoadSource(s), "source");
    }
    // Blend draws main augmentation AND external current: deepest; External lacks 3 s I_dyn.
    CHECK(minR[2] < minR[0] && minR[2] < minR[1], "Blend not deepest: main %.4f ext %.4f blend %.4f", minR[0], minR[1], minR[2]);
    std::printf("Selector precedence at Sag 1, depth 1, -18 dBFS bus: rail minimum Main %.4f, External %.4f, Blend %.4f\n", minR[0],
                minR[1], minR[2]);
}

void rtTrapControl() {
    cd::test::RtTrapScope trap;
    void* p = ::operator new(16);
    ::operator delete(p);
    CHECK(trap.counts().allocations == 1 && trap.counts().deallocations == 1, "RtTrap positive control not trapped");
}

}  // namespace

int main() {
    supplyBasics();
    partitions();
    t15();
    t16();
    selector();
    rtTrapControl();
    CHECK(g_rtCount == 0, "audio-path allocations/deallocations: %ld", g_rtCount);
    std::printf("UX-09 RtTrap over every supply/detector audio call: %ld allocations+deallocations; positive control trapped\n", g_rtCount);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("runtime_supply: all checks passed\n");
    return 0;
}
