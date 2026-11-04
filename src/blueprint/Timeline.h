// CircuitDrift blueprint: Hardware Lifecycle timeline (CD-F18, 03-blueprints "Lifecycle timeline").
// A scalar u in [0,1] interpolates CONDITION (not elapsed running time) between four explicit
// anchors at positions {0,1/3,2/3,1}. Each interval uses smoothstep h(t)=t*t*(3-2t); positive
// physical values interpolate in log space, signed/zero-crossing values linearly. The condition
// is a pure function of (scenario, u): scrubbing back and forth accumulates no damage. The
// replacement-generation counter (identity seeds) changes only in commitRestoration().
// Anchor numbers are the spec's PROPOSED creative table, not measured ageing data (G01 open).
// Threading: evaluateCondition is pure/noexcept; LifecycleTimeline setters, tick and fill are
// noexcept and allocation free (audio-safe); snapshots are plain values.
#pragma once
#include <cstdint>
#include <string_view>

namespace cd::blueprint {

enum class Scenario : unsigned char { Default, Alternate, Restored };
constexpr int kScenarioCount = 3;
constexpr int kAnchorCount = 4;

struct ConditionAnchor {
    double ageYears, capacitanceMult, esrMult, leakageFraction, toleranceSigma, biasOffsetSpan, noiseAddDb, driftMult;
};

struct ScenarioDef {
    const char* name;
    const char* labels[kAnchorCount];
    double years[kAnchorCount];            // label years (display year interpolates linearly between them)
    ConditionAnchor anchors[kAnchorCount];
};
const ScenarioDef& scenarioDef(Scenario s) noexcept;
constexpr double anchorPosition(int k) noexcept { return k == 0 ? 0.0 : k == 1 ? 1.0 / 3.0 : k == 2 ? 2.0 / 3.0 : 1.0; }

struct Condition {
    double displayYear = 0;
    ConditionAnchor values{};
    int intervalIndex = 0;                 // 0..2
};

// Pure. u is clamped to [0,1]; a non-finite u evaluates as 0.
Condition evaluateCondition(Scenario s, double u) noexcept;
double smoothstep(double t) noexcept;

struct TimelineSnapshot {
    Scenario scenario = Scenario::Default;
    double target = 0, smoothed = 0, rampStep = 0;
    long rampLeft = 0;
    std::uint64_t replacementGeneration = 0;
};

class LifecycleTimeline {
public:
    static constexpr double kSmoothingSeconds = 0.05;   // 50 ms linear ramp to every new target
    // Worker-side configuration (not audio): sample rate must be finite in [8 kHz, 768 kHz].
    bool prepare(double sampleRate) noexcept;
    bool setScenario(Scenario s) noexcept;               // condition changes continuously via smoothing; no reseed
    // Sample-timestamped automation: call at the sample where the new value takes effect.
    // Non-finite values are rejected (returns false, state unchanged); values clamp to [0,1].
    bool setTarget(double u) noexcept;
    double tick() noexcept;                              // advance one sample, returns smoothed u
    void fill(double* out, int frames) noexcept;         // frames <= 0 or null out is a no-op
    double smoothedU() const noexcept { return state_.smoothed; }
    double targetU() const noexcept { return state_.target; }
    Scenario scenario() const noexcept { return state_.scenario; }
    Condition condition() const noexcept { return evaluateCondition(state_.scenario, state_.smoothed); }
    Condition targetCondition() const noexcept { return evaluateCondition(state_.scenario, state_.target); }
    // Apply Restoration: allowed only when the target is the last anchor (u == 1). Returns false
    // otherwise, leaving everything unchanged. Increments the replacement generation exactly once.
    bool commitRestoration() noexcept;
    std::uint64_t replacementGeneration() const noexcept { return state_.replacementGeneration; }
    // Identity seed of a component: depends on base seed, component id and replacement generation only.
    std::uint64_t identitySeed(std::uint64_t baseSeed, std::string_view componentId) const noexcept;
    // Undo support: one scrub gesture = one snapshot.
    TimelineSnapshot snapshot() const noexcept { return state_; }
    void restore(const TimelineSnapshot& s) noexcept;
private:
    TimelineSnapshot state_;
    long rampSamples_ = 2400;
};

}  // namespace cd::blueprint
