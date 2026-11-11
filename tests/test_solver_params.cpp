// MnaSolver bounded parameter-update API (prerequisite for temperature feedback, G01 stays open).
// Every circuit is a labelled synthetic software fixture; no physical validation is claimed.
// updateParameter()/parameter() are allocation-free by construction (they only write the
// prepare-sized element and stamp tables); this suite checks behaviour, not allocation counts.
#include "dsp/MnaSolver.h"
#include "dsp/DeviceManifest.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace cd::dsp;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)

constexpr double kH = 1.0 / 192000;

NetElement source(int node, double value) {
    NetElement e;
    e.kind = NetElement::Kind::VoltageSource;
    e.a = node;
    e.value = value;
    return e;
}

NetElement device(const char* id, int a, int b, double p0, double p1 = 0, double p2 = 0) {
    NetElement e;
    e.model = findSyntheticFixture(id);
    e.a = a;
    e.b = b;
    e.param[0] = p0; e.param[1] = p1; e.param[2] = p2;
    return e;
}

// Elements: 0 = V source (node 1), 1 = R (1-2), 2 = C (2-0).
Netlist rc(double r, double c) {
    Netlist n;
    n.nodes = 2;
    n.elements = {source(1, 0), device("fixture.resistor", 1, 2, r), device("fixture.capacitor", 2, 0, c)};
    return n;
}

// Elements: 0 = V source, 1 = R, 2/3 = antiparallel diodes, 4 = C.
Netlist clipper() {
    Netlist n;
    n.nodes = 2;
    n.elements = {source(1, 0), device("fixture.resistor", 1, 2, 1000),
                  device("fixture.diode", 2, 0, 1e-14, 1, 0.025852), device("fixture.diode", 0, 2, 1e-14, 1, 0.025852),
                  device("fixture.capacitor", 2, 0, 1e-8)};
    return n;
}

std::vector<double> state(const MnaSolver& s) {
    std::vector<double> v(std::size_t(s.stateSize()));
    s.getState(v.data());
    return v;
}

void rejectsInvalidUpdates() {
    MnaSolver fresh;
    CHECK(!fresh.updateParameter(1, 0, 1000));
    CHECK(std::isnan(fresh.parameter(1, 0)));

    MnaSolver s;
    std::string err;
    CHECK(s.prepare(clipper(), kH, {}, err));
    s.setSource(0, 0.3);
    CHECK(s.step());
    const auto before = state(s);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    struct Bad { int element, k; double value; };
    const Bad bad[] = {
        {-1, 0, 1000}, {5, 0, 1000}, {99, 0, 1000},          // element out of range
        {0, 0, 1.0},                                          // source element is not a device
        {1, -1, 1000}, {1, 1, 1000}, {2, 3, 1e-14},           // parameter index out of range
        {1, 0, nan}, {1, 0, inf}, {1, 0, -inf},               // non-finite
        {1, 0, 1e-4}, {1, 0, 2e9}, {1, 0, 0}, {1, 0, -1000},  // resistor outside [1e-3, 1e9]
        {4, 0, 0}, {4, 0, 2.0}, {4, 0, 1e-16},                // capacitor outside [1e-15, 1]
        {2, 0, 1e-5}, {2, 1, 0.4}, {2, 1, 5}, {2, 2, 0.05}, {3, 2, 0.01}, {2, 0, nan},  // diode bounds
    };
    for (const Bad& b : bad) {
        const double old = s.parameter(b.element, b.k);
        CHECK(!s.updateParameter(b.element, b.k, b.value));
        CHECK(std::isnan(old) ? std::isnan(s.parameter(b.element, b.k)) : s.parameter(b.element, b.k) == old);
        CHECK(state(s) == before);
    }
    CHECK(std::isnan(s.parameter(0, 0)) && std::isnan(s.parameter(1, 1)) && std::isnan(s.parameter(-1, 0)));
    CHECK(s.parameter(1, 0) == 1000 && s.parameter(2, 2) == 0.025852 && s.parameter(4, 0) == 1e-8);
    // Boundary values themselves are accepted.
    CHECK(s.updateParameter(1, 0, 1e-3) && s.parameter(1, 0) == 1e-3);
    CHECK(s.updateParameter(1, 0, 1e9) && s.parameter(1, 0) == 1e9);
    CHECK(state(s) == before);  // an update never touches the dynamic state by itself
}

// v_k = (q_{k-1}/h + u/R) / (C/h + 1/R), q_k = C v_k: the exact backward-Euler recurrence.
void rcTimeConstantFollowsUpdate() {
    MnaSolver s;
    std::string err;
    double R = 10000, C = 1e-8;
    CHECK(s.prepare(rc(R, C), kH, {}, err));
    const double u = 1.0;
    s.setSource(0, u);
    double v = 0, q = 0, worst = 0;
    auto run = [&](int steps) {
        for (int k = 0; k < steps; ++k) {
            CHECK(s.step());
            v = (q / kH + u / R) / (C / kH + 1.0 / R);
            q = C * v;
            worst = std::max(worst, std::fabs(s.voltage(2) - v));
        }
    };
    run(200);
    // Per-step decay of (u - v) is a = (C/h)/(C/h + 1/R); measure it before and after R changes.
    auto decay = [&]() {
        const double e0 = u - s.voltage(2);
        CHECK(s.step());
        v = (q / kH + u / R) / (C / kH + 1.0 / R); q = C * v;
        worst = std::max(worst, std::fabs(s.voltage(2) - v));
        return (u - s.voltage(2)) / e0;
    };
    const double a1 = decay();
    CHECK(std::fabs(a1 - (C / kH) / (C / kH + 1 / R)) <= 1e-9);
    CHECK(s.updateParameter(1, 0, 2500));  // tau = RC drops 4x
    CHECK(s.parameter(1, 0) == 2500);
    R = 2500;
    const double a2 = decay();
    CHECK(std::fabs(a2 - (C / kH) / (C / kH + 1 / R)) <= 1e-9);
    CHECK(a2 < a1);
    run(100);
    // Capacitance update: the stored charge is kept, so the first step uses q_old = C_old * v_old.
    CHECK(s.updateParameter(2, 0, 4e-8));
    C = 4e-8;
    s.setSource(0, 0.0);
    double vv = s.voltage(2), qq = q;
    for (int k = 0; k < 100; ++k) {
        CHECK(s.step());
        vv = (qq / kH) / (C / kH + 1.0 / R);
        qq = C * vv;
        worst = std::max(worst, std::fabs(s.voltage(2) - vv));
    }
    CHECK(worst <= 1e-12);
    CHECK(s.failures() == 0);
}

// An update is indistinguishable from preparing with that value (same state, same steps).
void updateMatchesFreshPreparation() {
    MnaSolver updated, direct;
    std::string err;
    CHECK(updated.prepare(rc(10000, 1e-8), kH, {}, err));
    CHECK(direct.prepare(rc(4700, 2.2e-8), kH, {}, err));
    CHECK(state(updated) == state(direct));  // zero source: both DC points are 0 V
    CHECK(updated.updateParameter(1, 0, 4700) && updated.updateParameter(2, 0, 2.2e-8));
    for (int k = 0; k < 300; ++k) {
        const double u = std::sin(0.01 * k);
        updated.setSource(0, u); direct.setSource(0, u);
        CHECK(updated.step() && direct.step());
    }
    CHECK(state(updated) == state(direct));

    MnaSolver copy = updated;  // copies own their parameters
    CHECK(copy.updateParameter(1, 0, 100));
    CHECK(updated.parameter(1, 0) == 4700 && copy.parameter(1, 0) == 100);
}

void diodeUpdateTakesEffectAtNextStep() {
    MnaSolver base, hot;
    std::string err;
    CHECK(base.prepare(clipper(), kH, {}, err));
    hot = base;
    base.setSource(0, 2.0); hot.setSource(0, 2.0);
    for (int k = 0; k < 400; ++k) CHECK(base.step() && hot.step());
    CHECK(state(base) == state(hot));
    // Larger thermal voltage (a synthetic warmer diode) raises the clipping level.
    CHECK(hot.updateParameter(2, 2, 0.03) && hot.updateParameter(3, 2, 0.03));
    CHECK(state(base) == state(hot));  // nothing changes until the next step
    for (int k = 0; k < 400; ++k) CHECK(base.step() && hot.step());
    CHECK(hot.voltage(2) > base.voltage(2) + 0.05);
    CHECK(hot.updateParameter(2, 0, 1e-12) && hot.updateParameter(2, 1, 1.5));
    for (int k = 0; k < 50; ++k) CHECK(hot.step());
    CHECK(std::isfinite(hot.voltage(2)) && hot.failures() == 0 && base.failures() == 0);
}
}  // namespace

int main() {
    rejectsInvalidUpdates();
    rcTimeConstantFollowsUpdate();
    updateMatchesFreshPreparation();
    diodeUpdateTakesEffectAtNextStep();
    std::printf(failures ? "SOLVER PARAMS FAILED (%d)\n" : "SOLVER PARAMS PASSED\n", failures);
    return failures ? 1 : 0;
}
