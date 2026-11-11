// T18: Hardware Lifecycle timeline (CD-F18). Anchor values are the spec's PROPOSED creative table,
// not measured component ageing data.
#include "blueprint/Timeline.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace {
using namespace cd::blueprint;
void values(const ConditionAnchor& a, double* o);
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)

bool rel(double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(std::fabs(b), 1e-300) || a == b; }
bool bitsEq(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
// Field-wise bit equality (no memcmp over the whole struct: padding bytes are indeterminate).
bool sameBits(const Condition& a, const Condition& b) {
    double va[9], vb[9];
    values(a.values, va); values(b.values, vb);
    va[8] = a.displayYear; vb[8] = b.displayYear;
    for (int i = 0; i < 9; ++i) if (!bitsEq(va[i], vb[i])) return false;
    return a.intervalIndex == b.intervalIndex;
}

const double kTable[5][8] = {
    {0, 1, 1, 0, 0.0033, 0, 0, 1},         // Factory
    {15, 0.95, 1.4, 0.01, 0.02, 0.015, 3, 1.5},
    {35, 0.8, 3, 0.08, 0.05, 0.04, 9, 3},
    {51, 0.65, 6, 0.2, 0.08, 0.08, 15, 5},
    {5, 1, 1, 0, 0.0033, 0, 0, 1}};        // Restored ending

void values(const ConditionAnchor& a, double* o) {
    o[0] = a.ageYears; o[1] = a.capacitanceMult; o[2] = a.esrMult; o[3] = a.leakageFraction;
    o[4] = a.toleranceSigma; o[5] = a.biasOffsetSpan; o[6] = a.noiseAddDb; o[7] = a.driftMult;
}

void t18Anchors() {
    const Scenario all[] = {Scenario::Default, Scenario::Alternate, Scenario::Restored};
    const int rowOf[3][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 4}};
    const double years[3][4] = {{1975, 1990, 2010, 2026}, {1972, 1985, 2005, 2026}, {1975, 1985, 2005, 2026}};
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k < 4; ++k) {
            const Condition c = evaluateCondition(all[s], anchorPosition(k));
            double v[8];
            values(c.values, v);
            for (int j = 0; j < 8; ++j) CHECK(rel(v[j], kTable[rowOf[s][k]][j]));
            CHECK(std::fabs(c.displayYear - years[s][k]) < 1e-9);
        }
    // Labels (scenario metadata)
    CHECK(std::strcmp(scenarioDef(Scenario::Default).labels[0], "1975 Factory Spec") == 0);
    CHECK(std::strcmp(scenarioDef(Scenario::Default).labels[3], "2026 Derelict") == 0);
    CHECK(std::strcmp(scenarioDef(Scenario::Restored).labels[3], "2026 Restored") == 0);
    // Interval midpoints: smoothstep(0.5) = 0.5; log mean for positive values, linear for signed/zero-based.
    const Condition m = evaluateCondition(Scenario::Default, 1.0 / 6.0);
    CHECK(rel(m.values.capacitanceMult, std::sqrt(1.0 * 0.95)));
    CHECK(rel(m.values.esrMult, std::sqrt(1.0 * 1.4)));
    CHECK(rel(m.values.toleranceSigma, std::sqrt(0.0033 * 0.02)));
    CHECK(rel(m.values.driftMult, std::sqrt(1.5)));
    CHECK(rel(m.values.biasOffsetSpan, 0.0075));
    CHECK(rel(m.values.leakageFraction, 0.005));
    CHECK(rel(m.values.noiseAddDb, 1.5));
    CHECK(std::fabs(m.displayYear - 1982.5) < 1e-9);           // display year is linear in the interval fraction
    CHECK(rel(smoothstep(0.25), 0.15625));
    // smoothstep gives zero slope at the anchors: tiny move around 1/3 changes the condition negligibly
    const Condition a = evaluateCondition(Scenario::Default, 1.0 / 3.0 - 1e-5), b = evaluateCondition(Scenario::Default, 1.0 / 3.0 + 1e-5);
    CHECK(std::fabs(a.values.esrMult - b.values.esrMult) < 1e-8);
    // Monotone degradation in the Default scenario sweep; all values finite and inside the anchor envelope.
    double prevC = 2, prevEsr = 0;
    for (int i = 0; i <= 3000; ++i) {
        const Condition c = evaluateCondition(Scenario::Default, i / 3000.0);
        CHECK(c.values.capacitanceMult <= prevC + 1e-12 && c.values.esrMult >= prevEsr - 1e-12);
        CHECK(std::isfinite(c.values.noiseAddDb) && c.values.capacitanceMult >= 0.65 - 1e-12 && c.values.esrMult <= 6 + 1e-12);
        prevC = c.values.capacitanceMult; prevEsr = c.values.esrMult;
    }
    // Sad inputs
    CHECK(sameBits(evaluateCondition(Scenario::Default, std::nan("")), evaluateCondition(Scenario::Default, 0)));
    CHECK(sameBits(evaluateCondition(Scenario::Default, 1e300), evaluateCondition(Scenario::Default, 1)));
    CHECK(sameBits(evaluateCondition(Scenario::Default, -std::numeric_limits<double>::infinity()), evaluateCondition(Scenario::Default, 0)));
}

void t18ReversibleScrub() {
    LifecycleTimeline t;
    CHECK(t.prepare(48000));
    std::mt19937_64 rng(18);
    std::uniform_real_distribution<double> uni(0, 1);
    const double home = 0.4321;
    CHECK(t.setTarget(home));
    for (int i = 0; i < 4000; ++i) t.tick();
    const Condition ref = t.condition();
    const auto seed0 = t.identitySeed(99, "C7");
    for (int round = 0; round < 200; ++round) {          // random back-and-forth scrubbing, 20 ms apart
        CHECK(t.setTarget(uni(rng)));
        for (int i = 0; i < 960; ++i) t.tick();
    }
    CHECK(t.setTarget(home));
    for (int i = 0; i < 2400; ++i) t.tick();
    CHECK(t.smoothedU() == home);
    CHECK(sameBits(t.condition(), ref));                  // identical condition: no accumulated damage
    CHECK(t.replacementGeneration() == 0 && t.identitySeed(99, "C7") == seed0);
    CHECK(sameBits(t.condition(), evaluateCondition(Scenario::Default, home)));
}

void t18Smoothing() {
    LifecycleTimeline t;
    CHECK(!t.prepare(std::nan("")) && !t.prepare(100) && t.prepare(48000));
    CHECK(!t.setTarget(std::nan("")) && !t.setTarget(std::numeric_limits<double>::infinity()) && t.targetU() == 0);   // rejected, unchanged
    CHECK(t.setTarget(7) && t.targetU() == 1);                                          // clamped
    // linear 50 ms (2400 samples @48 kHz) ramp lands exactly on the target
    std::vector<double> ref(4800);
    t.fill(ref.data(), 4800);
    CHECK(std::fabs(ref[1199] - 0.5) < 1e-9 && ref[2399] == 1.0 && ref[2398] < 1.0 && ref[4799] == 1.0);
    CHECK(ref[0] > 0 && ref[0] < 1e-3);
    // block partition independence
    for (int block : {1, 17, 64, 127, 512, 8192}) {
        LifecycleTimeline u;
        u.prepare(48000);
        u.setTarget(1);
        std::vector<double> out(4800);
        for (int pos = 0; pos < 4800; pos += block) u.fill(out.data() + pos, std::min(block, 4800 - pos));
        CHECK(out == ref);
    }
    // zero/negative frames and null are no-ops
    const double before = t.smoothedU();
    t.fill(nullptr, 100);
    double dummy = -1;
    t.fill(&dummy, 0);
    t.fill(&dummy, -5);
    CHECK(dummy == -1 && t.smoothedU() == before);
    // retarget mid-ramp continues from the current value (no jump)
    LifecycleTimeline v;
    v.prepare(48000);
    v.setTarget(1);
    for (int i = 0; i < 1000; ++i) v.tick();
    const double mid = v.smoothedU();
    v.setTarget(0);
    CHECK(std::fabs(v.tick() - mid) < 1e-3);
}

void t18Replacement() {
    LifecycleTimeline t;
    t.prepare(48000);
    t.setScenario(Scenario::Restored);
    const char* ids[] = {"C1", "C2", "R1", "V1"};
    std::uint64_t before[4];
    for (int k = 0; k < 4; ++k) before[k] = t.identitySeed(1234, ids[k]);
    CHECK(before[0] != before[1] && before[1] != before[2]);            // per-component seeds differ
    t.setTarget(0.5);
    CHECK(!t.commitRestoration() && t.replacementGeneration() == 0);     // only at the last anchor
    t.setTarget(1.0);
    for (int i = 0; i < 3000; ++i) t.tick();
    for (int k = 0; k < 4; ++k) CHECK(t.identitySeed(1234, ids[k]) == before[k]);   // preview never reseeds
    CHECK(t.commitRestoration() && t.replacementGeneration() == 1);
    std::uint64_t after[4];
    for (int k = 0; k < 4; ++k) { after[k] = t.identitySeed(1234, ids[k]); CHECK(after[k] != before[k]); }
    // scrub away and back, switch scenario: identity unchanged until the next commit
    t.setTarget(0.2);
    for (int i = 0; i < 3000; ++i) t.tick();
    t.setScenario(Scenario::Default);
    t.setTarget(1.0);
    for (int i = 0; i < 3000; ++i) t.tick();
    for (int k = 0; k < 4; ++k) CHECK(t.identitySeed(1234, ids[k]) == after[k]);
    CHECK(t.identitySeed(1235, "C1") != after[0]);                       // base seed participates
    CHECK(t.commitRestoration() && t.replacementGeneration() == 2 && t.identitySeed(1234, "C1") != after[0]);
}

void t18Undo() {
    LifecycleTimeline t;
    t.prepare(48000);
    std::mt19937_64 rng(2018);
    std::vector<TimelineSnapshot> history;
    history.push_back(t.snapshot());
    for (int step = 0; step < 20; ++step) {           // one scrub gesture or commit per undo transaction
        if (step % 7 == 6) { t.setTarget(1.0); for (int i = 0; i < 2500; ++i) t.tick(); t.commitRestoration(); }
        else { t.setTarget(double(rng() % 1001) / 1000.0); for (int i = 0; i < 2500; ++i) t.tick(); }
        history.push_back(t.snapshot());
    }
    CHECK(history.size() == 21 && t.replacementGeneration() == 2);
    for (int step = 19; step >= 0; --step) {          // undo 20 times
        t.restore(history[std::size_t(step)]);
        const TimelineSnapshot s = t.snapshot();
        CHECK(s.target == history[std::size_t(step)].target && s.smoothed == history[std::size_t(step)].smoothed &&
              s.replacementGeneration == history[std::size_t(step)].replacementGeneration);
        CHECK(sameBits(t.condition(), evaluateCondition(Scenario::Default, history[std::size_t(step)].smoothed)));
    }
    CHECK(t.replacementGeneration() == 0 && t.smoothedU() == 0);
    TimelineSnapshot bad = history[3];
    bad.target = std::nan("");
    t.restore(bad);                                    // invalid snapshot ignored
    CHECK(t.replacementGeneration() == 0 && t.smoothedU() == 0);
}

}  // namespace

int main() {
    t18Anchors();
    t18ReversibleScrub();
    t18Smoothing();
    t18Replacement();
    t18Undo();
    if (failures) std::printf("blueprint_timeline: %d FAILED\n", failures);
    else std::printf("blueprint_timeline: all checks passed\n");
    return failures ? 1 : 0;
}
