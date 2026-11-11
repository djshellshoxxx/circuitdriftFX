// T06 macro composition tests. Pure-function checks; bindings are synthetic, no hardware data.
#include "control/Macros.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace cd::control;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)
bool near(double a, double b, double t = 1e-6) { return std::fabs(a - b) <= t; }
const double kNan = std::numeric_limits<double>::quiet_NaN();

ManifestBindings full() {
    ManifestBindings m;
    m.hasAge = m.hasSag = m.hasWarmth = m.hasDrift = true;
    m.analogNoise = true; m.cBase = 1e-3; m.cMin = 1e-4; m.biasSpan = 2.0; m.leakRange = 0.5;
    return m;
}

void endpoints() {  // T06 endpoint mappings
    const BaseValues base; const auto mb = full();
    Effective e0 = evaluate(base, {0, 0, 0, 0}, mb);
    CHECK(near(e0.serviceAgeYears, 0) && near(e0.tolSigma, 0) && near(e0.leakFrac, 0) && near(e0.shelfGainDb, 0));
    CHECK(near(e0.reservoirC, 1e-3, 1e-12) && near(e0.loadMultiplier, 1) && near(e0.driveDb, 0));
    CHECK(near(e0.fluxFactor, 1) && near(e0.driftSensitivity, 1) && near(e0.noiseDbfs, -90));
    Effective e1 = evaluate(base, {1, 1, 1, 1}, mb);
    CHECK(near(e1.serviceAgeYears, 60) && near(e1.tolSigma, 0.08) && near(e1.leakFrac, 0.05));
    CHECK(near(e1.shelfGainDb, -6) && near(e1.shelfHz, 6000) && near(e1.shelfQ, 0.707) && e1.noiseEnabled && near(e1.noiseDbfs, -66));
    CHECK(near(e1.reservoirC, 2.5e-4, 1e-12) && near(e1.loadMultiplier, 4));
    CHECK(near(e1.driveDb, 18) && near(e1.biasOffset, 0.16) && near(e1.fluxFactor, 1.5) && near(e1.driftSensitivity, 5));
    Effective eh = evaluate(base, {.5, 1, 0.5, .5}, mb);
    CHECK(near(eh.serviceAgeYears, 30) && near(eh.leakFrac, 0.1 * .25 * .5) && near(eh.driveDb, 9));
    auto m2 = mb; m2.cMin = 5e-4;  // Cmin floor
    CHECK(near(evaluate(base, {0, 1, 0, 0}, m2).reservoirC, 5e-4, 1e-12));
    BaseValues old; old.serviceAgeYears = 80;  // max(base, 60a)
    CHECK(near(evaluate(old, {1, 0, 0, 0}, mb).serviceAgeYears, 80));
    auto m3 = mb; m3.analogNoise = false;  // noise gated by flag
    CHECK(!evaluate(base, {1, 0, 0, 0}, m3).noiseEnabled);
    ModulationInput up{1}, dn{-1};  // drift motion +-0.15 d oct, +-1.5 d ms at q=+-1
    CHECK(near(evaluate(base, {0, 0, 1, 0}, mb, {nullptr, &up, nullptr}).cutoffMotionOct, 0.15));
    CHECK(near(evaluate(base, {0, 0, 1, 0}, mb, {nullptr, &dn, nullptr}).delayDevMs, -1.5));
    CHECK(near(evaluate(base, {0, 0, 0, 0}, mb, {nullptr, &up, nullptr}).delayDevMs, 0));
    CHECK(near(evaluate(base, {kNan, 2, -1, kNan}, mb).loadMultiplier, 4));  // NaN->0, clamp
    const auto c = designHighShelf(48000, 6000, -6, 0.707);  // RBJ shelf: DC unity, Nyquist -6 dB
    CHECK(near((c.b0 + c.b1 + c.b2) / (1 + c.a1 + c.a2), 1.0, 1e-9));
    CHECK(near((c.b0 - c.b1 + c.b2) / (1 - c.a1 + c.a2), std::pow(10, -6.0 / 20), 1e-9));
    const auto id = designHighShelf(48000, 30000, -6, 0.707);  // invalid f0 -> identity
    CHECK(id.b0 == 1 && id.a1 == 0);
}

void composition() {  // order: base -> timeline -> macros -> faults -> clamp
    BaseValues base; base.tolSigma = 0.2;
    TimelineOffsets tl; tl.tolSigma = 0.03; tl.driveDb = 2;
    FaultOverlay f; f.loadMultiplierScale = 0.5; f.leakFracAdd = 2.0;
    const auto e = evaluate(base, {1, 1, 0, 0}, full(), {&tl, nullptr, &f});
    CHECK(near(e.tolSigma, 0.25));       // 0.2+0.03+0.08 clamped to 0.25
    CHECK(near(e.loadMultiplier, 2.0));  // 4 * 0.5
    CHECK(near(e.leakFrac, 1.0));        // clamped
    CHECK(near(e.driveDb, 2));
    using S = SidechainSource;  // sidechain selector precedence
    CHECK(near(composeLoadCurrent(S::Main, 1, 0.5, 1, 9), 2.5));
    CHECK(near(composeLoadCurrent(S::External, 1, 0.5, 1, 0.7), 1.7));
    CHECK(near(composeLoadCurrent(S::Blend, 1, 0.5, 1, 0.7), 3.2));
    CHECK(near(composeLoadCurrent(S::External, 1, 0.5, 1, kNan), 1.0));
}

void baseUnchanged() {  // T06 base never written back
    BaseValues base; base.serviceAgeYears = 3; base.tolSigma = .01; base.driveDb = 1; base.fluxFactor = 1.1;
    const BaseValues copy = base;
    (void)evaluate(base, {1, 1, 1, 1}, full());
    CHECK(std::memcmp(&base, &copy, sizeof base) == 0);
}

void missingBinding() {  // T06 missing binding disables macro (neutral, flagged unsupported)
    ManifestBindings none;
    none.cBase = 1e-3; none.cMin = 1e-4; none.biasSpan = 1;
    const Effective e = evaluate({}, {1, 1, 1, 1}, none);
    CHECK(!e.supportsAge && !e.supportsSag && !e.supportsWarmth && !e.supportsDrift);
    CHECK(e.serviceAgeYears == 0 && e.shelfGainDb == 0 && !e.noiseEnabled && near(e.reservoirC, 1e-3, 1e-12));
    CHECK(e.loadMultiplier == 1 && e.driveDb == 0 && e.biasOffset == 0 && e.fluxFactor == 1);
    CHECK(e.driftSensitivity == 1 && e.cutoffMotionOct == 0 && e.delayDevMs == 0);
    auto only = none; only.hasSag = true;
    const Effective s = evaluate({}, {1, 1, 1, 1}, only);
    CHECK(s.supportsSag && near(s.loadMultiplier, 4) && s.driveDb == 0 && s.serviceAgeYears == 0);
}

// Targets change at fixed absolute samples, independent of how the stream is chunked into blocks.
std::vector<double> run(std::size_t bs, double fs) {
    const std::size_t N = 20000;
    MacroSmoother sm; const double init[2] = {0, 1};
    sm.prepare(fs, init, 2);
    std::vector<double> out;
    for (std::size_t n = 0; n < N;) {
        const std::size_t len = std::min(bs, N - n);
        for (std::size_t i = 0; i < len; ++i, ++n) {
            if (n == 100) { const double t[2] = {1, 0}; sm.setTargets(t, 2); }
            if (n == 9000) { const double t[2] = {.25, kNan}; sm.setTargets(t, 2); }
            sm.step(); out.push_back(sm.value(0)); out.push_back(sm.value(1));
        }
    }
    return out;
}

void smoothing() {  // T06 partition invariance, 20 ms time constant
    const double fs = 48000;
    const auto ref = run(1, fs);
    for (std::size_t bs : {17u, 64u, 127u, 512u, 8192u}) {
        const auto o = run(bs, fs); double peak = 0;
        for (std::size_t i = 0; i < ref.size(); ++i) peak = std::max(peak, std::fabs(o[i] - ref[i]));
        CHECK(peak <= 1e-6);
    }
    CHECK(near(ref[2 * (100 + 959)], 1 - std::exp(-1.0), 1e-3));  // one tau (960 samples) after step
    CHECK(std::isfinite(ref.back()) && near(ref.back(), 0.0, 1e-3));  // NaN target ignored
}

}  // namespace

int main() {
    endpoints(); composition(); baseUnchanged(); missingBinding(); smoothing();
    std::puts(failures ? "FAILED" : "control macros ok");
    return failures ? 1 : 0;
}
