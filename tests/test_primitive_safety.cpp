// Numeric containment and linked-power regressions. Fixtures are synthetic, not hardware data.
#include "dsp/ThermalNetwork.h"
#include "dsp/OutputProtection.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace cd::dsp;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)
const double kInf = std::numeric_limits<double>::infinity();
const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kMax = std::numeric_limits<double>::max();

ThermalNetwork::Config twoNodes() {
    ThermalNetwork::Config c;
    c.n = 2;
    c.capacity[0] = 2;
    c.capacity[1] = 1;
    c.toAmbient[0] = .2;
    c.toAmbient[1] = .3;
    c.coupling[0][1] = c.coupling[1][0] = .5;
    return c;
}

std::array<double, 2> temperatures(const ThermalNetwork& n) {
    return {n.temperature(0), n.temperature(1)};
}

void thermalContainment() {
    auto c = twoNodes();
    ThermalNetwork net;
    CHECK(net.prepare(c, .001, 298.15));
    const double p[] = {1, .5};
    net.step(p);
    const auto before = temperatures(net);
    ThermalNetwork control = net;
    CHECK(!net.prepare(c, .001, kInf));
    CHECK(temperatures(net) == before);
    auto huge = c;
    huge.capacity[0] = kMax;
    CHECK(!net.prepare(huge, .001, 298.15));
    CHECK(temperatures(net) == before);
    huge = c;
    huge.n = 3;
    huge.capacity[2] = 1;
    huge.toAmbient[2] = .2;
    huge.coupling[0][1] = huge.coupling[1][0] = 1e308;
    huge.coupling[0][2] = huge.coupling[2][0] = 1e308;
    CHECK(!net.prepare(huge, 1, 1));
    CHECK(net.size() == 2 && temperatures(net) == before);

    for (const double invalid : {kInf, kNan, 0.0, -1.0, kMax}) {
        const double bad[] = {301, invalid};
        net.setState(bad);
        CHECK(temperatures(net) == before);
    }
    net.setState(nullptr);
    net.step(nullptr);
    CHECK(temperatures(net) == before);
    for (const double invalidPower : {kInf, kNan, -kMax}) {
        const double bad[] = {1, invalidPower};
        net.step(bad);
        CHECK(temperatures(net) == before);
    }
    net.step(p);
    control.step(p);
    CHECK(temperatures(net) == temperatures(control));

    const double largeState[] = {1e304, 1e304};
    net.setState(largeState);
    CHECK(temperatures(net) == (std::array<double, 2>{1e304, 1e304}));
    const auto largeBefore = temperatures(net);
    const double overflowPower[] = {kMax, kMax};
    net.step(overflowPower);
    CHECK(temperatures(net) == largeBefore);

    ThermalNetwork::Config tiny;
    tiny.n = 1;
    tiny.capacity[0] = tiny.toAmbient[0] = 2e-300;
    ThermalNetwork extreme;
    CHECK(extreme.prepare(tiny, 1, 298.15));
    const double overflowingProduct = 1e10;
    extreme.step(&overflowingProduct);
    CHECK(extreme.temperature(0) == 298.15);
}

void thermalReference() {
    ThermalNetwork::Config c;
    c.n = 1;
    c.capacity[0] = 2;
    c.toAmbient[0] = .2;
    ThermalNetwork net;
    CHECK(net.prepare(c, .001, 298.15));
    const double power = 1;
    for (int i = 0; i < 10000; ++i) net.step(&power);
    const double exact = 298.15 + 5 * (1 - std::exp(-1.0));
    CHECK(std::fabs(net.temperature(0) - exact) <= .002);
    const double before = net.temperature(0);
    const double cooling = -.1;
    net.step(&cooling);
    CHECK(net.temperature(0) > 0 && net.temperature(0) < before);
}

std::array<double, 7> filterState(const ButterworthHpf& f) {
    return {f.b0, f.b1, f.b2, f.a1, f.a2, f.z1, f.z2};
}

void hpfContainment() {
    ButterworthHpf f;
    CHECK(f.design(48000, 15));
    f.process(.5);
    const auto before = filterState(f);
    for (const auto pair : {std::array<double, 2>{kInf, 15}, {kNan, 15}, {48000, kInf},
                           {48000, kNan}, {0, 15}, {48000, 24000}}) {
        CHECK(!f.design(pair[0], pair[1]));
        CHECK(filterState(f) == before);
    }
    // A finite normalized ratio must not overflow in pi*fc before dividing by fs.
    ButterworthHpf large, normal;
    CHECK(large.design(1.7e308, .4 * 1.7e308));
    CHECK(normal.design(48000, .4 * 48000));
    const auto a = filterState(large), b = filterState(normal);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(std::isfinite(a[i]) && std::fabs(a[i] - b[i]) < 1e-14);
}

void linkedGain() {
    GainMatcher mono, powers;
    mono.prepare(48000);
    powers.prepare(48000);
    for (int i = 0; i < 10000; ++i) {
        const double dry = .3 * std::sin(.03 * i), wet = .6 * std::sin(.03 * i);
        CHECK(mono.update(dry, wet) == powers.updateMeanSquares(dry * dry, wet * wet));
    }
    GainMatcher linked;
    linked.prepare(48000);
    // Channels (.2,.6) dry and (.4,1.2) wet: the linked RMS ratio is 1/2, not 1/4.
    constexpr double dryPower = (.2 * .2 + .6 * .6) / 2;
    constexpr double wetPower = (.4 * .4 + 1.2 * 1.2) / 2;
    for (int i = 0; i < 5 * 48000; ++i) linked.updateMeanSquares(dryPower, wetPower);
    CHECK(std::fabs(20 * std::log10(2 * linked.gain())) <= .5);

    const double gain = linked.gain();
    GainMatcher control = linked;
    for (const auto bad : {std::array<double, 2>{-1, wetPower}, {dryPower, -1}, {kNan, wetPower},
                          {dryPower, kNan}, {kInf, wetPower}, {dryPower, kInf}}) {
        CHECK(linked.updateMeanSquares(bad[0], bad[1]) == gain);
    }
    CHECK(linked.update(1e308, .5) == gain);
    for (const double invalidRate : {kInf, kNan, 0.0, -48000.0}) linked.prepare(invalidRate);
    CHECK(linked.gain() == gain);
    for (int i = 0; i < 1000; ++i)
        CHECK(linked.updateMeanSquares(dryPower, wetPower) == control.updateMeanSquares(dryPower, wetPower));

    GainMatcher quiet;
    quiet.prepare(48000);
    for (int i = 0; i < 48000; ++i) quiet.updateMeanSquares(1e-8, 4e-8);
    CHECK(quiet.gain() == 1);
}
} // namespace

int main() {
    thermalContainment();
    thermalReference();
    hpfContainment();
    linkedGain();
    std::printf(failures ? "PRIMITIVE SAFETY FAILED (%d)\n" : "PRIMITIVE SAFETY PASSED\n", failures);
    return failures ? 1 : 0;
}
