#include "blueprint/Timeline.h"
#include <cmath>

namespace cd::blueprint {
namespace {

// PROPOSED creative anchor table (03-blueprints spec). Columns: age, C, ESR, leakage, sigma, bias, noise dB, drift.
constexpr ConditionAnchor kFactory = {0, 1, 1, 0, 0.0033, 0, 0, 1};
constexpr ConditionAnchor kTour = {15, 0.95, 1.4, 0.01, 0.02, 0.015, 3, 1.5};
constexpr ConditionAnchor kStorage = {35, 0.8, 3, 0.08, 0.05, 0.04, 9, 3};
constexpr ConditionAnchor kDerelict = {51, 0.65, 6, 0.2, 0.08, 0.08, 15, 5};
constexpr ConditionAnchor kRestored = {5, 1, 1, 0, 0.0033, 0, 0, 1};

const ScenarioDef kScenarios[kScenarioCount] = {
    {"Default", {"1975 Factory Spec", "1990 Tour-Worn", "2010 Attic Storage", "2026 Derelict"},
     {1975, 1990, 2010, 2026}, {kFactory, kTour, kStorage, kDerelict}},
    {"Alternate", {"1972 Factory Spec", "1985 Tour-Worn", "2005 Attic Storage", "2026 Derelict"},
     {1972, 1985, 2005, 2026}, {kFactory, kTour, kStorage, kDerelict}},
    {"Restored", {"1975 Factory Spec", "1985 Tour-Worn", "2005 Attic Storage", "2026 Restored"},
     {1975, 1985, 2005, 2026}, {kFactory, kTour, kStorage, kRestored}},
};

double lerp(double a, double b, double t) noexcept { return t <= 0 ? a : t >= 1 ? b : a + (b - a) * t; }
// Positive physical values: interpolate the logarithm. Exact at the endpoints.
double logLerp(double a, double b, double t) noexcept {
    if (t <= 0) return a;
    if (t >= 1) return b;
    return std::exp(std::log(a) + (std::log(b) - std::log(a)) * t);
}

std::uint64_t mix64(std::uint64_t z) noexcept {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace

const ScenarioDef& scenarioDef(Scenario s) noexcept {
    const auto k = std::size_t(s);
    return kScenarios[k < std::size_t(kScenarioCount) ? k : 0];
}

double smoothstep(double t) noexcept {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3.0 - 2.0 * t);
}

Condition evaluateCondition(Scenario s, double u) noexcept {
    const ScenarioDef& def = scenarioDef(s);
    if (!std::isfinite(u)) u = 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    int k = int(u * 3.0);
    if (k > 2) k = 2;
    if (k < 0) k = 0;
    // Keep interval boundaries exact: u == 1/3 or 2/3 may fall on either side of the cast.
    double tl = (u - anchorPosition(k)) * 3.0;
    if (tl < 0 && k > 0) { --k; tl = (u - anchorPosition(k)) * 3.0; }
    const double t = tl < 0 ? 0 : tl > 1 ? 1 : tl;
    const double h = smoothstep(t);
    const ConditionAnchor &a = def.anchors[k], &b = def.anchors[k + 1];
    Condition c;
    c.intervalIndex = k;
    c.displayYear = lerp(def.years[k], def.years[k + 1], t);  // linear between label years
    c.values.ageYears = lerp(a.ageYears, b.ageYears, h);
    c.values.capacitanceMult = logLerp(a.capacitanceMult, b.capacitanceMult, h);
    c.values.esrMult = logLerp(a.esrMult, b.esrMult, h);
    c.values.leakageFraction = lerp(a.leakageFraction, b.leakageFraction, h);  // starts at 0: linear
    c.values.toleranceSigma = logLerp(a.toleranceSigma, b.toleranceSigma, h);
    c.values.biasOffsetSpan = lerp(a.biasOffsetSpan, b.biasOffsetSpan, h);     // signed offset: linear
    c.values.noiseAddDb = lerp(a.noiseAddDb, b.noiseAddDb, h);                 // already logarithmic, starts at 0
    c.values.driftMult = logLerp(a.driftMult, b.driftMult, h);
    return c;
}

bool LifecycleTimeline::prepare(double sampleRate) noexcept {
    if (!(std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 768000.0)) return false;
    rampSamples_ = long(std::llround(kSmoothingSeconds * sampleRate));
    if (rampSamples_ < 1) rampSamples_ = 1;
    state_.rampLeft = 0;
    state_.smoothed = state_.target;
    return true;
}

bool LifecycleTimeline::setScenario(Scenario s) noexcept {
    if (std::size_t(s) >= std::size_t(kScenarioCount)) return false;
    state_.scenario = s;
    return true;
}

bool LifecycleTimeline::setTarget(double u) noexcept {
    if (!std::isfinite(u)) return false;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    state_.target = u;
    state_.rampLeft = rampSamples_;
    state_.rampStep = (u - state_.smoothed) / double(rampSamples_);
    return true;
}

double LifecycleTimeline::tick() noexcept {
    if (state_.rampLeft > 0) {
        if (--state_.rampLeft == 0) state_.smoothed = state_.target;
        else state_.smoothed += state_.rampStep;
    }
    return state_.smoothed;
}

void LifecycleTimeline::fill(double* out, int frames) noexcept {
    if (!out) return;
    for (int i = 0; i < frames; ++i) out[i] = tick();
}

bool LifecycleTimeline::commitRestoration() noexcept {
    if (state_.target != 1.0) return false;
    ++state_.replacementGeneration;
    return true;
}

std::uint64_t LifecycleTimeline::identitySeed(std::uint64_t baseSeed, std::string_view componentId) const noexcept {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a over the stable component ID
    for (unsigned char ch : componentId) { h ^= ch; h *= 1099511628211ull; }
    return mix64(mix64(baseSeed) ^ mix64(h) ^ mix64(state_.replacementGeneration * 0xD1B54A32D192ED03ull + 1));
}

void LifecycleTimeline::restore(const TimelineSnapshot& s) noexcept {
    if (!(std::isfinite(s.target) && std::isfinite(s.smoothed) && std::isfinite(s.rampStep)) || s.rampLeft < 0 ||
        std::size_t(s.scenario) >= std::size_t(kScenarioCount))
        return;
    state_ = s;
}

}  // namespace cd::blueprint
