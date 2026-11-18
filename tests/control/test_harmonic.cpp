// T14: harmonic targeting on a SYNTHETIC memoryless asymmetric waveshaper (NOT a physical device model;
// G01 stays open). Proves map calibration, branch selection, >=6 dB directional H2/H3 shift, bounds,
// smoothing and "no map -> disabled".
#include <algorithm>
#include <cmath>
#include <vector>
#include "TestHarness.h"
#include "control/ControlProcessor.h"
#include "control/HarmonicTargeting.h"

using namespace cd;
using namespace cd::control;

constexpr double kFs = 48000.0;

// SYNTHETIC fixture: y = tanh(g*(x + 0.5*bias)); bias breaks symmetry (even harmonics), drive adds odd.
static bool syntheticRender(double bias, double driveDb, double f0, double levelDbfs, std::vector<double>& out) {
    const double amp = std::pow(10.0, levelDbfs / 20.0), g = std::pow(10.0, driveDb / 20.0);
    const std::size_t n = std::size_t(std::lround(kFs / f0)) * 20;  // 20 whole cycles
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = amp * std::sin(2.0 * std::numbers::pi * f0 * double(i) / kFs);
        out[i] = std::tanh(g * (x + 0.5 * bias));
    }
    return true;
}

static double measuredR(double bias, double driveDb, double f0 = 1000.0, double lvl = -12.0) {
    std::vector<double> v;
    syntheticRender(bias, driveDb, f0, lvl, v);
    return analyzeHarmonics(v.data(), v.size(), kFs, f0).ratioDb();
}

static void analyzer() {
    // Known-harmonic signal: H1=1, H2=0.1, H3=0.05.
    const std::size_t n = 48000;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2 * std::numbers::pi * 1000.0 * double(i) / kFs;
        x[i] = std::sin(ph) + 0.1 * std::sin(2 * ph + 0.3) + 0.05 * std::sin(3 * ph + 1.0) + 0.02;
    }
    const HarmonicSpectrum s = analyzeHarmonics(x.data(), n, kFs, 1000.0);
    CHECK(std::fabs(s.h[1] - 1.0) < 1e-3 && std::fabs(s.h[2] - 0.1) < 1e-3 && std::fabs(s.h[3] - 0.05) < 1e-3, "H1..H3 %g %g %g", s.h[1], s.h[2], s.h[3]);
    CHECK(std::fabs(s.dc - 0.02) < 1e-3, "dc %g", s.dc);
    CHECK(std::fabs(s.ratioDb() - 20 * std::log10(2.0)) < 0.05, "R %g", s.ratioDb());
    CHECK(s.h[9] < 1e-4, "H9 empty");
    std::vector<double> bad = {0, 1, NAN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!analyzeHarmonics(bad.data(), bad.size(), kFs, 1000).finite, "non-finite flagged");
}

static HarmonicMap& theMap() {
    static HarmonicMap map;
    static bool built = false;
    if (!built) {
        map.setAxes(-1.0, 1.0, 0.0, 36.0);
        CalibrationConfig cfg;
        cfg.fs = kFs;
        cfg.f0Hz = {100.0, 1000.0};
        cfg.levelsDbfs = {-12.0};
        const int valid = calibrateHarmonicMap(map, syntheticRender, cfg);
        CHECK(valid > 0 && valid == map.validCount(), "valid points %d", valid);
        std::printf("harmonic: synthetic map valid points %d / %d\n", valid, HarmonicMap::kBias * HarmonicMap::kDrive);
        built = true;
    }
    return map;
}

static void directionalShift() {
    const HarmonicMap& map = theMap();
    for (double baseDrive : {6.0, 12.0, 24.0}) {
        const HarmonicSelection odd = selectHarmonicTarget(map, -1.0, 0.0, baseDrive);
        const HarmonicSelection even = selectHarmonicTarget(map, +1.0, 0.0, baseDrive);
        const HarmonicSelection mid = selectHarmonicTarget(map, 0.0, 0.0, baseDrive);
        CHECK(odd.available && even.available && mid.available, "available at drive %g", baseDrive);
        for (const auto* s : {&odd, &even, &mid}) {
            CHECK(std::fabs(s->driveDb - baseDrive) <= 6.0 + 1e-9, "drive bound: %g vs base %g", s->driveDb, baseDrive);
            CHECK(s->bias >= -1.0 && s->bias <= 1.0, "bias span");
        }
        // Verify against fresh renders at the selected operating points (not just map lookups).
        const double rOdd = measuredR(odd.bias, odd.driveDb), rEven = measuredR(even.bias, even.driveDb);
        CHECK(rEven - rOdd >= 6.0, "drive %g: directional shift %.2f dB (odd %.2f even %.2f)", baseDrive, rEven - rOdd, rOdd, rEven);
        CHECK(odd.rTarget < mid.rTarget && mid.rTarget < even.rTarget, "ordered targets");
        // Monotone along the target control.
        double prev = -1e9;
        for (double t = -1.0; t <= 1.0001; t += 0.25) {
            const double r = selectHarmonicTarget(map, t, 0.0, baseDrive).rTarget;
            CHECK(r >= prev - 1e-9, "monotone target %g", t);
            prev = r;
        }
        // Rendered selections are valid (no DC-excess / unstable).
        for (const auto* s : {&odd, &even}) {
            std::vector<double> v;
            syntheticRender(s->bias, s->driveDb, 1000.0, -12.0, v);
            const HarmonicSpectrum sp = analyzeHarmonics(v.data(), v.size(), kFs, 1000.0);
            CHECK(sp.finite && std::fabs(sp.dc) <= 0.5 * sp.h[1], "no DC-excess at selection (dc %g h1 %g)", sp.dc, sp.h[1]);
        }
    }
}

static void invalidPointsNeverSelected() {
    HarmonicMap m;
    m.setAxes(-1.0, 1.0, 0.0, 36.0);
    CalibrationConfig cfg;
    cfg.fs = kFs;
    cfg.f0Hz = {1000.0};
    cfg.levelsDbfs = {-12.0};
    // Fixture that fails (NaN) for bias > 0.6 and returns false for drive < 3 dB.
    auto render = [](double bias, double drive, double f0, double lvl, std::vector<double>& out) {
        if (drive < 3.0) return false;
        syntheticRender(bias, drive, f0, lvl, out);
        if (bias > 0.6) out[3] = NAN;
        return true;
    };
    calibrateHarmonicMap(m, render, cfg);
    for (int i = 0; i < HarmonicMap::kBias; ++i)
        for (int j = 0; j < HarmonicMap::kDrive; ++j)
            if (m.biasAt(i) > 0.6 || m.driveAt(j) < 3.0) CHECK(!m.valid(i, j), "invalid point flagged %d %d", i, j);
    const HarmonicSelection s = selectHarmonicTarget(m, 1.0, 0.0, 12.0);
    CHECK(s.available && s.bias <= 0.6 + 1e-9, "selection stays on valid points: %g", s.bias);
    // A flat/empty map has no usable branch.
    HarmonicMap empty;
    CHECK(!selectHarmonicTarget(empty, 1.0, 0.0, 12.0).available, "empty map unavailable");
}

static void runtimeBehavior() {
    const HarmonicMap& map = theMap();
    HarmonicTargeting ht;
    CHECK(!ht.available() && !ht.update(1.0, 1.0, 0.0, 12.0, 0.01) && ht.biasDelta() == 0 && ht.driveDeltaDb() == 0,
          "no map -> disabled, zero deltas");
    ht.setMap(&map);
    ht.update(1.0, 0.0, 0.0, 12.0, 1.0);
    CHECK(ht.biasDelta() == 0 && ht.driveDeltaDb() == 0, "amount 0 -> no change");
    // 50 ms smoothing: after 50 ms the delta has covered 1-1/e of the way.
    HarmonicTargeting a;
    a.setMap(&map);
    const HarmonicSelection sel = selectHarmonicTarget(map, 1.0, 0.0, 12.0);
    for (int i = 0; i < 10; ++i) a.update(1.0, 1.0, 0.0, 12.0, 0.005);
    CHECK(std::fabs(a.biasDelta() / sel.bias - (1 - std::exp(-1.0))) < 1e-6, "bias smoothing 50 ms: %g", a.biasDelta() / sel.bias);
    for (int i = 0; i < 200; ++i) a.update(1.0, 1.0, 0.0, 12.0, 0.01);
    CHECK(std::fabs(a.biasDelta() - sel.bias) < 1e-6 && std::fabs(a.driveDeltaDb() - (sel.driveDb - 12.0)) < 1e-6, "settles to selection");
    // Amount interpolates from the base point.
    HarmonicTargeting half;
    half.setMap(&map);
    for (int i = 0; i < 300; ++i) half.update(1.0, 0.5, 0.0, 12.0, 0.01);
    CHECK(std::fabs(half.biasDelta() - 0.5 * sel.bias) < 1e-6, "amount 0.5 halves the move");
}

static void processorIntegration() {
    const HarmonicMap& map = theMap();
    ControlProcessor cp;
    cp.prepare(kFs, 480, 3);
    core::ParamValues p = core::defaultValues();
    p[core::indexOf("harmonics.target")] = 1.0;
    p[core::indexOf("harmonics.amount")] = 1.0;
    p[core::indexOf("slot.a.stage.drive")] = 12.0;
    p[core::indexOf("slot.a.stage.bias")] = 0.0;
    p[core::indexOf("slot.b.stage.drive")] = 12.0;
    core::ModulationBlock mb;
    ControlContext ctx;
    ctx.frames = 480;
    for (int i = 0; i < 50; ++i) cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    CHECK(!cp.status().harmonicAvailable[0] && !cp.status().harmonicAvailable[1], "no map: status disabled");
    CHECK(mb.slot[0].biasOffset == 0.0 && mb.slot[0].driveOffsetDb == 0.0, "no map: no contribution");
    cp.setHarmonicMap(0, &map);
    for (int i = 0; i < 100; ++i) cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    CHECK(cp.harmonicMapInUse(0) == &map, "map picked up at block boundary");
    CHECK(cp.status().harmonicAvailable[0] && !cp.status().harmonicAvailable[1], "per-slot availability");
    const HarmonicSelection sel = selectHarmonicTarget(map, 1.0, 0.0, 12.0);
    CHECK(std::fabs(mb.slot[0].biasOffset - sel.bias) < 1e-6 && std::fabs(mb.slot[0].driveOffsetDb - (sel.driveDb - 12.0)) < 1e-6,
          "slot A offsets follow selection");
    CHECK(mb.slot[1].biasOffset == 0.0, "slot B untouched");
    CHECK(std::fabs(mb.slot[0].driveOffsetDb) <= 6.0 + 1e-9, "drive delta bounded +-6 dB");
    // Warmth adds to (not replaces) the targeting delta.
    p[core::indexOf("macro.warmth")] = 1.0;
    for (int i = 0; i < 100; ++i) cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    CHECK(std::fabs(mb.slot[0].biasOffset - (0.08 + sel.bias)) < 1e-6, "warmth + targeting bias");
}

int main() {
    analyzer();
    directionalShift();
    invalidPointsNeverSelected();
    runtimeBehavior();
    processorIntegration();
    if (g_fail) { std::printf("harmonic: %d failure(s)\n", g_fail); return 1; }
    std::printf("harmonic: all passed\n");
    return 0;
}
