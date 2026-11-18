// T06: macro endpoint mappings, one-way composition, block-partition agreement (control level).
#include <algorithm>
#include <cmath>
#include <vector>
#include "TestHarness.h"
#include "control/ControlProcessor.h"
#include "control/MacroMapper.h"

using namespace cd;
using namespace cd::control;

static void endpoints() {
    for (int mask = 0; mask < 16; ++mask) {
        for (int noise = 0; noise < 2; ++noise) {
            MacroInputs in;
            in.age = (mask & 1) ? 1.0 : 0.0;
            in.sag = (mask & 2) ? 1.0 : 0.0;
            in.drift = (mask & 4) ? 1.0 : 0.0;
            in.warmth = (mask & 8) ? 1.0 : 0.0;
            in.analogNoiseEnabled = noise != 0;
            core::SlotModulation m;
            applyMacros(in, m);
            const double a = in.age, s = in.sag, d = in.drift, w = in.warmth;
            CHECK(std::fabs(m.ageYearsMin - 60 * a) < 1e-6, "age years");
            CHECK(std::fabs(m.toleranceSigmaAdd - 0.08 * a) < 1e-6, "tol");
            CHECK(std::fabs(m.leakageAdd - 0.1 * a * a) < 1e-6, "leak");
            CHECK(std::fabs(m.hfShelfDb + 6 * a) < 1e-6, "shelf");
            CHECK(std::fabs(m.ageNoiseDbfs - (noise ? -90 + 24 * a : -200.0)) < 1e-6, "noise %g", m.ageNoiseDbfs);
            CHECK(std::fabs(m.reservoirCapDivisor - (1 + 3 * s)) < 1e-6, "cap");
            CHECK(std::fabs(m.sagMacro - s) < 1e-6, "sag");
            CHECK(std::fabs(m.driveOffsetDb - 18 * w) < 1e-6, "drive");
            CHECK(std::fabs(m.biasOffset - 0.08 * w) < 1e-6, "bias");
            CHECK(std::fabs(m.fluxDriveFactor - (1 + 0.5 * w)) < 1e-6, "flux");
            CHECK(std::fabs(m.thermalSensitivityScale - (1 + 4 * d)) < 1e-6, "thermal");
        }
    }
    // Midpoints (a=0.5): leakage is quadratic.
    MacroInputs h{0.5, 0.5, 0.5, 0.5, true};
    core::SlotModulation m;
    applyMacros(h, m);
    CHECK(std::fabs(m.leakageAdd - 0.025) < 1e-9 && std::fabs(m.ageNoiseDbfs + 78.0) < 1e-9, "midpoint");
    // Out of range / NaN inputs clamp.
    MacroInputs bad{-3.0, 7.0, NAN, 2.0, false};
    applyMacros(bad, m);
    CHECK(m.ageYearsMin == 0 && m.sagMacro == 1.0 && m.thermalSensitivityScale == 1.0 && m.driveOffsetDb == 18.0, "clamp");
    // Defaults are the identity.
    MacroInputs zero;
    applyMacros(zero, m);
    core::SlotModulation id;
    CHECK(m.ageYearsMin == id.ageYearsMin && m.reservoirCapDivisor == 1 && m.fluxDriveFactor == 1 &&
          m.ageNoiseDbfs == -200, "zero macros identity");
}

struct Snapshot {
    core::SlotModulation s[2];
    std::vector<double> load, trans;
};

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static double maxDiff(const core::SlotModulation& a, const core::SlotModulation& b) {
    double d = 0;
    auto up = [&](double x, double y) { d = std::max(d, std::fabs(x - y)); };
    up(a.ageYearsMin, b.ageYearsMin); up(a.toleranceSigmaAdd, b.toleranceSigmaAdd); up(a.leakageAdd, b.leakageAdd);
    up(a.hfShelfDb, b.hfShelfDb); up(a.ageNoiseDbfs, b.ageNoiseDbfs); up(a.reservoirCapDivisor, b.reservoirCapDivisor);
    up(a.sagMacro, b.sagMacro); up(a.driveOffsetDb, b.driveOffsetDb); up(a.biasOffset, b.biasOffset);
    up(a.fluxDriveFactor, b.fluxDriveFactor); up(a.thermalSensitivityScale, b.thermalSensitivityScale);
    up(a.creativeCutoffOct, b.creativeCutoffOct); up(a.creativeDelayMs, b.creativeDelayMs);
    up(a.recoveryTauS, b.recoveryTauS);
    return d;
}

// Piecewise-constant parameters change only at 8192-sample segment boundaries; compare the state after
// each segment across block partitions.
static void partitions() {
    constexpr double fs = 48000.0;
    constexpr int segLen = 8192, segs = 6, total = segLen * segs;
    std::vector<double> in(total), sc(total);
    th::Lcg rng;
    for (int i = 0; i < total; ++i) {
        in[std::size_t(i)] = 0.5 * std::sin(0.01 * i) * (i % 20000 < 3000 ? 1.0 : 0.05) + 0.02 * rng.bipolar();
        sc[std::size_t(i)] = 0.3 * std::sin(0.05 * i);
    }
    const int parts[] = {1, 17, 64, 127, 512, 8192};
    std::vector<std::vector<Snapshot>> results;
    for (int part : parts) {
        ControlProcessor cp;
        cp.prepare(fs, 8192, 0xABCDEFu);
        cp.setSidechainBudgetA(0.01);
        std::vector<Snapshot> snaps;
        Snapshot cur;
        int pos = 0;
        for (int seg = 0; seg < segs; ++seg) {
            core::ParamValues p = core::defaultValues();
            const double ramp = seg / double(segs - 1);
            p[core::indexOf("macro.age")] = ramp;
            p[core::indexOf("macro.sag")] = 1.0 - ramp;
            p[core::indexOf("macro.drift")] = 0.3 + 0.7 * ramp;
            p[core::indexOf("macro.warmth")] = 0.5 * ramp;
            p[core::indexOf("analogNoise.enabled")] = seg % 2;
            p[core::indexOf("tempo.enabled")] = 1;
            p[core::indexOf("tempo.depth")] = 0.8;
            p[core::indexOf("tempo.division")] = 2;
            p[core::indexOf("transient.preserve")] = 0.9;
            p[core::indexOf("sidechain.source")] = 2;
            p[core::indexOf("sidechain.depth")] = 1.0;
            const core::ParamValues pCopy = p;
            for (int done = 0; done < segLen;) {
                const int n = std::min(part, segLen - done);
                const double* ch[2] = {in.data() + pos, in.data() + pos};
                const double* sch[2] = {sc.data() + pos, sc.data() + pos};
                AudioView mainV{ch, 2}, scV{sch, 2};
                ControlContext ctx;
                ctx.frames = n;
                ctx.sampleStart = uint64_t(pos);
                ctx.bpm = 100.0 + 10.0 * seg;  // tempo change at segment boundaries; no PPQ => free-run
                core::ModulationBlock mb;
                cp.processBlock(p, ctx, mainV, &scV, mb);
                cur.s[0] = mb.slot[0];
                cur.s[1] = mb.slot[1];
                cur.load.insert(cur.load.end(), mb.externalLoadA, mb.externalLoadA + n);
                cur.trans.insert(cur.trans.end(), mb.transientPreserve, mb.transientPreserve + n);
                done += n;
                pos += n;
            }
            CHECK(p == pCopy, "processBlock must not write parameters");
            snaps.push_back(cur);
        }
        results.push_back(std::move(snaps));
    }
    for (std::size_t r = 1; r < results.size(); ++r) {
        for (int seg = 0; seg < segs; ++seg) {
            for (int s = 0; s < 2; ++s) {
                const double d = maxDiff(results[0][std::size_t(seg)].s[s], results[r][std::size_t(seg)].s[s]);
                CHECK(d <= 1e-6, "partition %d seg %d slot %d diff %g", parts[r], seg, s, d);
            }
        }
        const auto& a = results[0].back();
        const auto& b = results[r].back();
        double dl = 0, dt = 0;
        for (std::size_t i = 0; i < a.load.size(); ++i) {
            dl = std::max(dl, std::fabs(a.load[i] - b.load[i]));
            dt = std::max(dt, std::fabs(a.trans[i] - b.trans[i]));
        }
        CHECK(dl <= 1e-9 && dt <= 1e-9, "per-sample buffers differ: load %g trans %g (part %d)", dl, dt, parts[r]);
    }
}

static void composition() {
    ControlProcessor cp;
    cp.prepare(48000.0, 512, 1);
    core::ParamValues p = core::defaultValues();
    p[core::indexOf("macro.warmth")] = 1.0;
    p[core::indexOf("macro.age")] = 1.0;
    core::ModulationBlock mb;
    ControlContext ctx;
    ctx.frames = 64;
    cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    for (int s = 0; s < 2; ++s) {
        CHECK(near(mb.slot[s].driveOffsetDb, 18.0, 1e-6) && near(mb.slot[s].biasOffset, 0.08, 1e-6), "warmth endpoints via processor");
        CHECK(near(mb.slot[s].ageYearsMin, 60.0, 1e-6), "age via processor");
        CHECK(mb.slot[s].creativeCutoffOct == 0 && mb.slot[s].creativeDelayMs == 0 && !mb.slot[s].creativeDelayActive &&
                  mb.slot[s].recoveryTauS == 0 && mb.slot[s].sidechainMode == 0,
              "defaults contribute nothing");
    }
    CHECK(mb.externalLoadA && mb.transientPreserve, "buffers present");
    for (int i = 0; i < 64; ++i) CHECK(mb.externalLoadA[i] == 0.0 && mb.transientPreserve[i] == 0.0, "idle buffers zero");
}

int main() {
    endpoints();
    partitions();
    composition();
    if (g_fail) { std::printf("macros: %d failure(s)\n", g_fail); return 1; }
    std::printf("macros: all passed\n");
    return 0;
}
