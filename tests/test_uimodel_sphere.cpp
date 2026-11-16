// Tests: UX-07 Tone State Sphere mappings, smoothing, Reduce Motion, render-RNG isolation.
// Engine is a SYNTHETIC fixture; no physical validation is claimed.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "runtime/Engine.h"
#include "uimodel/Sphere.h"

namespace {
using namespace cd::uimodel;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d [%s]: ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)
bool near(double a, double b, double e = 1e-12) { return std::fabs(a - b) <= e; }

void testMappings() {
    CHECK(railFraction(60, 120) == 0.5 && railFraction(300, 120) == 1.0 && railFraction(-5, 120) == 0.0, "r_fraction clamp");
    CHECK(railFraction(5, 0) == 1.0 && railFraction(std::nan(""), 100) == 1.0, "invalid V_nom/NaN -> no squash");
    CHECK(near(squashOf(0), 0.25) && near(squashOf(1), 0) && near(squashOf(0.5), 0.125), "squash");
    CHECK(near(baseRadius(-60), 0.85) && near(baseRadius(0), 0.95) && near(baseRadius(-30), 0.90) && near(baseRadius(-200), 0.85) && near(baseRadius(12), 0.95), "base radius");
    CHECK(near(verticalScale(0.25), 0.75) && near(horizontalScale(0.25), 1.125), "scales");
    CHECK(near(edgeAmplitudeOf(1, 1, 1.0), 0.03) && near(edgeAmplitudeOf(1, 0, 1.0), 0.015) && edgeAmplitudeOf(9, 9, 1.0) <= 0.03 + 1e-15, "edge <= 3%%");
    const Rgb c0 = toneColor(0), c5 = toneColor(0.5), c1 = toneColor(1);
    CHECK(c0.b > c0.r && c5.r > c5.b && c1.r > c1.g && toneColor(-3).r == c0.r && toneColor(9).r == c1.r, "cyan->amber->crimson");
    ToneInputs in;
    in.railVolts = 90; in.nominalRailVolts = 120; in.rmsDbfs = -30; in.age = 1; in.drift = 0.5; in.warmth = 0.7;
    SphereVisual v = mapVisual(in);
    CHECK(near(v.railFraction, 0.75) && near(v.squash, 0.0625) && near(v.radius, 0.90) && near(v.scaleV, 0.9375) && near(v.scaleH, 1.03125), "combined mapping");
    CHECK(v.controlPreview && near(v.colorT, 0.7), "no residual -> Warmth with control-preview flag");
    in.residualAvailable = true; in.residualNorm = 0.25;
    v = mapVisual(in);
    CHECK(!v.controlPreview && near(v.colorT, 0.25), "measured residual drives color, no preview flag");
    CHECK(near(v.edgeAmplitude, 0.03 * 0.90 * 0.75), "edge from 0.5*Age+0.5*Drift");
    // NaN inputs stay finite.
    ToneInputs bad;
    bad.railVolts = std::nan(""); bad.rmsDbfs = std::nan(""); bad.age = INFINITY; bad.warmth = std::nan("");
    v = mapVisual(bad);
    CHECK(std::isfinite(v.radius) && std::isfinite(v.colorT) && std::isfinite(v.edgeAmplitude) && v.silent, "non-finite inputs sanitized");
}

void testSilenceSmoothingMotion() {
    ToneInputs loud;
    loud.railVolts = 60; loud.nominalRailVolts = 120; loud.rmsDbfs = -10; loud.age = 1; loud.drift = 1; loud.sag = 0.5; loud.warmth = 0.4;
    ToneInputs silent = loud;
    silent.rmsDbfs = -120;
    SphereSmoother sm;
    sm.reset(mapVisual(loud));
    CHECK(sm.current().edgeAmplitude > 0.02 && sm.current().squash > 0.1, "active deformation");
    // 80 ms smoothing: after 80 ms a step response reaches ~63.2 %.
    const SphereVisual tgt = mapVisual(silent);
    const double s0 = sm.current().squash;
    sm.step(tgt, 0.080);
    const double frac = (s0 - sm.current().squash) / (s0 - tgt.squash);
    CHECK(near(frac, 1.0 - std::exp(-1.0), 1e-9), "tau = 80 ms (%.6f)", frac);
    for (int i = 0; i < 200; ++i) sm.step(tgt, 1.0 / 60.0);
    CHECK(sm.current().silent && sm.current().squash < 1e-6 && sm.current().edgeAmplitude < 1e-9 && near(sm.current().radius, 0.85, 1e-6), "silence holds a calm ring");
    CHECK(sm.current().sagIndicator == tgt.sagIndicator, "indicators follow");
    // monotone, no overshoot
    SphereSmoother m2;
    m2.reset(mapVisual(loud));
    double prev = m2.current().squash;
    bool mono = true;
    for (int i = 0; i < 100; ++i) { const double c = m2.step(tgt, 0.016).squash; mono = mono && c <= prev + 1e-15 && c >= -1e-15; prev = c; }
    CHECK(mono, "monotone smoothing");
    const double keep = m2.current().colorT;
    m2.step(tgt, 0.0); m2.step(tgt, -1.0); m2.step(tgt, std::nan(""));
    CHECK(m2.current().colorT == keep, "bad dt ignored");
    // Reduce Motion: no deformation/pulsing, indicators retained.
    ToneInputs rm = loud;
    rm.reduceMotion = true;
    const SphereVisual r = mapVisual(rm);
    CHECK(r.squash == 0 && r.edgeAmplitude == 0 && r.scaleV == 1 && r.scaleH == 1, "reduce motion disables deformation");
    CHECK(r.sagIndicator == 0.5 && near(r.warmthIndicator, 0.4), "numeric Sag/Warmth indicators retained");
    ToneInputs off = loud;
    off.enabled = false;
    CHECK(mapVisual(off).edgeAmplitude == 0, "animate only while enabled");
}

std::vector<double> renderEngine(bool useRenderRng) {
    cd::runtime::EngineConfig c;
    c.sampleRate = 48000; c.maxBlock = 256; c.channels = 1;
    c.initialMacros = {0.85, 0.65, 0.6, 0.45};
    cd::runtime::Engine eng;
    std::string err;
    if (!eng.prepare(c, err)) { ++g_fail; return {}; }
    RenderRng rng(0xC0FFEE);
    SphereSmoother sm;
    std::vector<double> out;
    for (int blk = 0; blk < 20; ++blk) {
        if (useRenderRng) {   // heavy render-side randomness between audio blocks
            ToneInputs ti;
            ti.rmsDbfs = -20; ti.age = 1; ti.drift = 1; ti.railVolts = 100; ti.nominalRailVolts = 120;
            for (int k = 0; k < 5000; ++k) (void)(rng.signedUnit() * sm.step(mapVisual(ti), 1.0 / 60.0).edgeAmplitude);
        }
        std::vector<double> in(256), o(256);
        for (int i = 0; i < 256; ++i) in[std::size_t(i)] = 0.5 * std::sin(2 * M_PI * 220.0 * double(blk * 256 + i) / 48000.0);
        const double* ip[1] = {in.data()};
        double* op[1] = {o.data()};
        cd::runtime::ProcessBlock b;
        b.ctx = {48000, std::int64_t(blk) * 256, 0, 256, false};
        b.buses = {1, ip, op};
        eng.process(b);
        out.insert(out.end(), o.begin(), o.end());
    }
    return out;
}

void testRngIsolation() {
    RenderRng a(1), b(1), c(2);
    CHECK(a.next() == b.next() && a.next() == b.next(), "deterministic");
    CHECK(RenderRng(1).next() != c.next(), "seed matters");
    double mn = 1, mx = -1;
    RenderRng u(7);
    for (int i = 0; i < 10000; ++i) { const double x = u.uniform(); mn = std::fmin(mn, x); mx = std::fmax(mx, x); }
    CHECK(mn >= 0 && mx < 1 && mx > 0.99 && mn < 0.01, "uniform range");
    const auto base = renderEngine(false);
    const auto with = renderEngine(true);
    bool same = base.size() == with.size() && !base.empty();
    for (std::size_t i = 0; same && i < base.size(); ++i) same = std::bit_cast<std::uint64_t>(base[i]) == std::bit_cast<std::uint64_t>(with[i]);
    CHECK(same, "UX-07: engine output bit-identical with heavy render-RNG use");
}
}  // namespace

int main() {
    testMappings();
    testSilenceSmoothingMotion();
    testRngIsolation();
    if (g_fail) { std::printf("uimodel sphere: %d FAILED\n", g_fail); return 1; }
    std::printf("uimodel sphere: all UX-07 checks passed\n");
    return 0;
}
