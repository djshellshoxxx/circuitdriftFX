// Numeric containment regressions; every circuit is a synthetic software fixture.
#include "dsp/MnaSolver.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace cd::dsp;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)

NetElement source(int node, double value) {
    NetElement e;
    e.kind = NetElement::Kind::VoltageSource;
    e.a = node;
    e.value = value;
    return e;
}

NetElement device(const char* id, int a, int b, double parameter) {
    NetElement e;
    e.model = findSyntheticFixture(id);
    e.a = a;
    e.b = b;
    e.param[0] = parameter;
    return e;
}

Netlist rc() {
    Netlist n;
    n.nodes = 2;
    n.elements = {source(1, 0), device("fixture.resistor", 1, 2, 10000),
                  device("fixture.capacitor", 2, 0, 1e-8)};
    return n;
}

std::vector<double> state(const MnaSolver& solver) {
    std::vector<double> result(solver.stateSize());
    solver.getState(result.data());
    return result;
}

void finitePreparation() {
    std::string error;
    const double inf = std::numeric_limits<double>::infinity();
    for (const double tol : {inf, -inf, std::numeric_limits<double>::quiet_NaN(), 0.0, -1.0, 1e-6}) {
        MnaSolver solver;
        CHECK(!solver.prepare(rc(), 1.0 / 192000, {8, tol}, error));
        CHECK(!error.empty() && solver.unknowns() == 0 && solver.stateSize() == 0 && !solver.step());
    }
    Netlist overflow;
    overflow.nodes = 2;
    overflow.elements = {source(1, 1e308), source(2, -1e308), device("fixture.capacitor", 1, 2, 1)};
    MnaSolver solver;
    CHECK(!solver.prepare(overflow, 1.0 / 192000, {}, error));
    CHECK(!error.empty() && solver.stateSize() == 0);

    overflow.nodes = 1;
    overflow.elements = {source(1, 1e308), device("fixture.resistor", 1, 0, .001)};
    CHECK(!solver.prepare(overflow, 1.0 / 192000, {}, error));
    CHECK(solver.stateSize() == 0);
    CHECK(!solver.prepare(rc(), std::numeric_limits<double>::denorm_min(), {}, error));
    CHECK(solver.stateSize() == 0);
}

void transactionalPreparationAndSteps() {
    std::string error;
    constexpr double h = 1.0 / 192000;
    MnaSolver solver;
    CHECK(solver.prepare(rc(), h, {}, error));
    solver.setSource(0, 1);
    double reference = 0;
    const double r = h / 1e-4;
    for (int i = 0; i < 64; ++i) {
        CHECK(solver.step());
        reference = (reference + r) / (1 + r);
        CHECK(std::fabs(solver.voltage(2) - reference) <= 1e-12);
    }
    MnaSolver continuation = solver;
    const auto before = state(solver);
    const auto beforeIterations = solver.lastIterations();
    const auto beforeResidual = solver.lastScaledResidual();
    Netlist overflow;
    overflow.nodes = 2;
    overflow.elements = {source(1, 1e308), source(2, -1e308), device("fixture.capacitor", 1, 2, 1)};
    CHECK(!solver.prepare(overflow, h, {}, error));
    CHECK(state(solver) == before);
    CHECK(solver.lastIterations() == beforeIterations && solver.lastScaledResidual() == beforeResidual);
    CHECK(!solver.prepare(rc(), h, {8, std::numeric_limits<double>::infinity()}, error));
    CHECK(state(solver) == before);
    CHECK(solver.step() && continuation.step());
    CHECK(state(solver) == state(continuation));

    const auto finite = state(solver);
    for (double sourceValue : {std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        solver.setSource(0, sourceValue);
        CHECK(!solver.step());
        CHECK(state(solver) == finite);
    }
    CHECK(solver.failures() == 3 && solver.needsFallback());
    solver.setSource(0, 1);
    CHECK(solver.step() && continuation.step());
    CHECK(!solver.needsFallback() && state(solver) == state(continuation));

    Netlist highCurrent;
    highCurrent.nodes = 1;
    highCurrent.elements = {source(1, 0), device("fixture.resistor", 1, 0, .001)};
    MnaSolver overloaded;
    CHECK(overloaded.prepare(highCurrent, h, {}, error));
    const auto beforeOverload = state(overloaded);
    overloaded.setSource(0, 1e308); // Finite voltage, but the branch current cannot be represented.
    CHECK(!overloaded.step());
    CHECK(state(overloaded) == beforeOverload && overloaded.failures() == 1);
}

void checkpointRejection() {
    MnaSolver solver;
    std::string error;
    CHECK(solver.prepare(rc(), 1.0 / 192000, {}, error));
    solver.setSource(0, 1);
    CHECK(solver.step());
    const auto before = state(solver);
    for (std::size_t i = 0; i < before.size(); ++i) {
        for (double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            auto corrupted = before;
            corrupted[i] = bad;
            solver.setState(corrupted.data());
            CHECK(state(solver) == before);
        }
    }
    auto overflow = before;
    overflow[0] = 1e308;
    overflow[1] = -1e308;
    solver.setState(overflow.data());
    CHECK(state(solver) == before);
    solver.setState(nullptr);
    CHECK(state(solver) == before);

    MnaSolver restored;
    CHECK(restored.prepare(rc(), 1.0 / 192000, {}, error));
    restored.setState(before.data());
    restored.setSource(0, 1);
    CHECK(restored.step() && solver.step());
    CHECK(state(restored) == state(solver));
}
} // namespace

int main() {
    finitePreparation();
    transactionalPreparationAndSteps();
    checkpointRejection();
    std::printf(failures ? "SOLVER SAFETY FAILED (%d)\n" : "SOLVER SAFETY PASSED\n", failures);
    return failures ? 1 : 0;
}
