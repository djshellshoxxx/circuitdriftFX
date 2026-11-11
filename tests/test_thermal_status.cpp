// CircuitDrift dsp: ThermalNetwork step-rejection status/counter regressions (T20 support,
// validation repair packet). Owner: cd_dsp tests. Threading: single-threaded test executable.
// Proves: every rejected step reports a bounded reason and increments counters while committing
// no temperature change (bit-exact, including no partial per-node update); accepted trajectories
// are unaffected by interleaved rejections; prepare/clear reset diagnostics; failed prepare does
// not. SYNTHETIC thermal constants only; no physical calibration (G01 open).
#include "dsp/ThermalNetwork.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
using cd::dsp::ThermalNetwork;
using Status = ThermalNetwork::StepStatus;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kMax = std::numeric_limits<double>::max();

ThermalNetwork::Config twoNodes() {
    ThermalNetwork::Config c;
    c.n = 2;
    c.capacity[0] = 1.0; c.capacity[1] = 2.0;
    c.toAmbient[0] = 0.5; c.toAmbient[1] = 0.25;
    c.coupling[0][1] = c.coupling[1][0] = 0.1;
    return c;
}
std::array<double, 2> temps(const ThermalNetwork& n) { return {n.temperature(0), n.temperature(1)}; }
bool bitEqual(const std::array<double, 2>& a, const std::array<double, 2>& b) { return std::memcmp(a.data(), b.data(), sizeof a) == 0; }

// One rejected step: expected status, counters advance, temperatures bit-identical.
void expectRejected(ThermalNetwork& net, const double* power, Status expected) {
    const auto before = temps(net);
    const auto rejected = net.rejectedSteps(), accepted = net.acceptedSteps(), run = net.consecutiveRejectedSteps();
    net.step(power);
    if (net.lastStepStatus() != expected) std::printf("  status %d expected %d\n", int(net.lastStepStatus()), int(expected));
    CHECK(net.lastStepStatus() == expected);
    CHECK(net.rejectedSteps() == rejected + 1 && net.acceptedSteps() == accepted && net.consecutiveRejectedSteps() == run + 1);
    CHECK(bitEqual(temps(net), before));
}

void T20_unprepared() {
    ThermalNetwork net;
    CHECK(net.lastStepStatus() == Status::None && net.acceptedSteps() == 0 && net.rejectedSteps() == 0);
    const double p = 1.0;
    net.step(&p);
    CHECK(net.lastStepStatus() == Status::NotPrepared && net.rejectedSteps() == 1 && net.size() == 0);
    net.step(nullptr);
    CHECK(net.lastStepStatus() == Status::NotPrepared && net.consecutiveRejectedSteps() == 2);
}

void T20_rejectionReasons() {
    ThermalNetwork net;
    CHECK(net.prepare(twoNodes(), 1e-3, 300.0));
    CHECK(net.lastStepStatus() == Status::None && net.rejectedSteps() == 0);
    const std::array<double, 2> heat{1.0, 0.5};
    net.step(heat.data());
    CHECK(net.lastStepStatus() == Status::Accepted && net.acceptedSteps() == 1 && net.consecutiveRejectedSteps() == 0);
    CHECK(net.temperature(0) > 300.0 && net.temperature(1) > 300.0);

    expectRejected(net, nullptr, Status::NullInput);
    const std::array<double, 2> nanSecond{1.0, kNaN}, infFirst{kInf, 0.0}, negInf{0.0, -kInf};
    expectRejected(net, nanSecond.data(), Status::NonFinitePower);  // node 0 finite: no partial update
    expectRejected(net, infFirst.data(), Status::NonFinitePower);
    expectRejected(net, negInf.data(), Status::NonFinitePower);
    // Node 1 alone driven negative: node 0 candidate is positive but must not be committed.
    const std::array<double, 2> coolSecond{0.0, -1e9};
    expectRejected(net, coolSecond.data(), Status::NonPositiveTemperature);
    CHECK(net.consecutiveRejectedSteps() == 5 && net.rejectedSteps() == 5);
    net.step(heat.data());
    CHECK(net.lastStepStatus() == Status::Accepted && net.consecutiveRejectedSteps() == 0 && net.rejectedSteps() == 5);

    // Finite-power overflow of the right-hand side (reviewed defect: previously silent).
    const std::array<double, 2> hot{1e300, 1e300};
    net.setState(hot.data());
    CHECK(bitEqual(temps(net), hot));
    const std::array<double, 2> maxPower{kMax, kMax};
    expectRejected(net, maxPower.data(), Status::Overflow);

    net.clearStepDiagnostics();
    CHECK(net.lastStepStatus() == Status::None && net.acceptedSteps() == 0 && net.rejectedSteps() == 0
          && net.consecutiveRejectedSteps() == 0 && bitEqual(temps(net), hot));
}

// Interleaved rejected steps never perturb the accepted trajectory (bit-exact).
void T20_rejectedStepsCommitNothing() {
    ThermalNetwork a, b;
    CHECK(a.prepare(twoNodes(), 1e-3, 300.0) && b.prepare(twoNodes(), 1e-3, 300.0));
    const std::array<double, 2> bad[] = {{kNaN, 1.0}, {0.0, -1e12}, {kInf, kInf}};
    for (int i = 0; i < 2000; ++i) {
        const std::array<double, 2> p{0.001 * (i % 17), -0.0005 * (i % 5)};
        a.step(p.data());
        b.step(bad[i % 3].data());
        b.step(nullptr);
        b.step(p.data());
        CHECK(b.lastStepStatus() == Status::Accepted);
    }
    CHECK(bitEqual(temps(a), temps(b)));
    CHECK(a.acceptedSteps() == 2000 && a.rejectedSteps() == 0);
    CHECK(b.acceptedSteps() == 2000 && b.rejectedSteps() == 4000 && b.consecutiveRejectedSteps() == 0);
    std::printf("T20 thermal: 4000 interleaved rejected steps, accepted trajectory bit-exact (T=%.6f K, %.6f K)\n",
                b.temperature(0), b.temperature(1));
}

void T20_prepareResetsDiagnostics() {
    ThermalNetwork net;
    CHECK(net.prepare(twoNodes(), 1e-3, 300.0));
    net.step(nullptr); net.step(nullptr);
    auto bad = twoNodes(); bad.toAmbient[0] = bad.toAmbient[1] = 0.0;  // ungrounded island
    CHECK(!net.prepare(bad, 1e-3, 300.0));
    CHECK(net.rejectedSteps() == 2 && net.lastStepStatus() == Status::NullInput && net.size() == 2);  // failed prepare: untouched
    CHECK(net.prepare(twoNodes(), 1e-3, 310.0));
    CHECK(net.rejectedSteps() == 0 && net.lastStepStatus() == Status::None && net.temperature(0) == 310.0);
    for (int i = 0; i < 100000; ++i) net.step(nullptr);  // bounded counters, no wrap in range
    CHECK(net.rejectedSteps() == 100000 && net.consecutiveRejectedSteps() == 100000 && net.temperature(1) == 310.0);
}
}  // namespace

int main() {
    T20_unprepared(); T20_rejectionReasons(); T20_rejectedStepsCommitNothing(); T20_prepareResetsDiagnostics();
    std::printf("thermal status regressions: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
