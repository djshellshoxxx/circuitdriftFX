// T14 (CD-F14) harmonic targeting. The map below is built from a SYNTHETIC asymmetric waveshaper;
// it is NOT a calibrated device map (gate G01 open). It proves the container/mapper/measurement
// mechanics only.
#include "control/HarmonicTarget.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace {
using namespace cd::control;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)
const double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kFs = 48000.0;

// SYNTHETIC fixture: asymmetric tanh waveshaper. Gain 2 at 0 dB drive.
double synth(double x, double bias, double driveDb) {
    const double g = 2.0 * std::pow(10.0, driveDb / 20.0);
    return std::tanh(g * (x + bias)) - std::tanh(g * bias);
}

HarmonicLevels measureAt(double bias, double driveDb, double dbfs) {
    return measureHarmonics([&](double x) { return synth(x, bias, driveDb); }, std::pow(10.0, dbfs / 20.0));
}

void measurementHelper() {
    // Pure odd nonlinearity: H2 ~ 0; cubic y = x + 0.1 x^3 gives H3 = 0.1*A^3/4.
    const double A = 0.5;
    auto odd = measureHarmonics([](double x) { return x + 0.1 * x * x * x; }, A);
    CHECK(odd.ok);
    CHECK(std::fabs(odd.amplitude[3] - 0.1 * A * A * A / 4.0) < 1e-9);
    CHECK(odd.amplitude[2] < 1e-12 && std::fabs(odd.dc) < 1e-12);
    CHECK(std::fabs(odd.amplitude[1] - (A + 0.075 * A * A * A)) < 1e-9);
    // Quadratic y = x + 0.2 x^2: H2 = 0.2 A^2 / 2, DC = 0.2 A^2 / 2.
    auto even = measureHarmonics([](double x) { return x + 0.2 * x * x; }, A);
    CHECK(std::fabs(even.amplitude[2] - 0.1 * A * A) < 1e-9 && std::fabs(even.dc - 0.1 * A * A) < 1e-9);
    // Stateful callable (one-pole) works and settles.
    double st = 0;
    auto lp = measureHarmonics([&](double x) { st += 0.2 * (x - st); return st; }, A, 256, 40, 8);
    CHECK(lp.ok && lp.amplitude[1] > 0.0);
    // Sad paths.
    CHECK(!measureHarmonics([](double) { return kNan; }, A).ok);
    CHECK(!measureHarmonics([](double) { return std::numeric_limits<double>::infinity(); }, A).ok);
    CHECK(!measureHarmonics(nullptr, A).ok);
    CHECK(!measureHarmonics([](double x) { return x; }, 0.0).ok);
    CHECK(!measureHarmonics([](double x) { return x; }, kNan).ok);
    CHECK(!measureHarmonics([](double x) { return x; }, A, 8).ok);
    CHECK(!measureHarmonics([](double x) { return x; }, A, 256, 0, 0).ok);
}

void missingMap() {
    HarmonicTarget t;
    CHECK(!t.configure(nullptr, 0.0, 0.0, kFs));
    CHECK(!t.enabled() && t.reason() == DisabledReason::MissingMap);
    CHECK(!t.setTarget(1.0, 1.0));
    t.step();  // harmless
    CalibrationMap empty;
    CHECK(!empty.usable());
    CHECK(!t.configure(&empty, 0.0, 0.0, kFs));
    CHECK(!t.enabled() && t.reason() == DisabledReason::NoStablePoints);
    CalibrationMap m;
    CHECK(!m.configure(0.5, 0.5, -12, 12, MapProvenance::Synthetic, "x"));
    CHECK(!m.configure(kNan, 1, -12, 12, MapProvenance::Synthetic, "x"));
    CHECK(m.configure(-0.4, 0.4, -12, 12, MapProvenance::Synthetic, "x"));
    CHECK(!m.setPoint(41, 0, MapPoint{}) && !m.setPoint(0, 17, MapPoint{}) && !m.setPoint(-1, 0, MapPoint{}));
    MapPoint nan;
    nan.ratioDb = kNan;
    CHECK(!m.setPoint(0, 0, nan));
    CHECK(!m.usable());  // no stable points yet
    // All unstable map built from a diverging callable.
    CHECK(!buildMap(m, [](double, double, double) { return kNan; }));
    CHECK(!m.usable());
    CHECK(!t.configure(&m, 0.0, 0.0, kFs) && t.reason() == DisabledReason::NoStablePoints);
}

void syntheticFixture() {
    CalibrationMap map;
    CHECK(map.configure(-0.4, 0.4, -12.0, 12.0, MapProvenance::Synthetic, "SYNTHETIC asymmetric tanh fixture; not a calibrated device"));
    CHECK(buildMap(map, synth));
    CHECK(!map.calibratedDevice() && map.provenance() == MapProvenance::Synthetic);
    int stable = 0;
    for (int i = 0; i < CalibrationMap::kBiasPoints; ++i)
        for (int j = 0; j < CalibrationMap::kDrivePoints; ++j) stable += map.point(i, j).stable ? 1 : 0;
    std::printf("T14 SYNTHETIC map: %d of %d points stable\n", stable, CalibrationMap::kBiasPoints * CalibrationMap::kDrivePoints);
    CHECK(stable > 0);

    HarmonicTarget t;
    CHECK(t.configure(&map, 0.2, 0.0, kFs));  // base bias 0.2, drive 0 dB
    CHECK(t.enabled() && t.reason() == DisabledReason::None);
    std::printf("T14 branch attainable R: %.2f .. %.2f dB\n", t.branchRatioMinDb(), t.branchRatioMaxDb());

    OperatingPoint odd, even, neutral;
    CHECK(t.setTarget(-1.0, 1.0)); odd = t.destination();
    CHECK(t.setTarget(1.0, 1.0)); even = t.destination();
    CHECK(t.setTarget(0.0, 1.0)); neutral = t.destination();
    // Bounds.
    for (const auto& p : {odd, even, neutral}) {
        CHECK(p.bias >= -0.4 - 1e-12 && p.bias <= 0.4 + 1e-12);
        CHECK(std::fabs(p.driveDb - 0.0) <= HarmonicTarget::kMaxDriveDeltaDb + 1e-12);
    }
    // Direction and size, measured on the real (synthetic) waveshaper, not just the map.
    double worstShift = 1e9;
    for (const double lvl : {-24.0, -12.0, -6.0}) {
        // R clamped to +/-60 dB like the map (an exactly-symmetric point has H2 at the 1e-12 numeric floor).
        auto clampR = [](double r) { return std::max(-60.0, std::min(60.0, r)); };
        const double rOdd = clampR(measureAt(odd.bias, odd.driveDb, lvl).ratioDb());
        const double rEven = clampR(measureAt(even.bias, even.driveDb, lvl).ratioDb());
        const double rNeu = clampR(measureAt(neutral.bias, neutral.driveDb, lvl).ratioDb());
        std::printf("T14 level %+.0f dBFS: R(odd bias=%.3f)=%.2f dB, R(neutral)=%.2f dB, R(even bias=%.3f)=%.2f dB, shift=%.2f dB\n",
                    lvl, odd.bias, rOdd, rNeu, even.bias, rEven, rEven - rOdd);
        worstShift = std::min(worstShift, rEven - rOdd);
        CHECK(rEven - rOdd >= 6.0);
        CHECK(rOdd <= rNeu + 1e-9 && rNeu <= rEven + 1e-9);  // directionally ordered
        // DC bounded at the selected points.
        for (const auto& p : {odd, even}) {
            const auto lv = measureAt(p.bias, p.driveDb, lvl);
            CHECK(lv.ok && std::fabs(lv.dc) / lv.amplitude[1] <= CalibrationMap::kDefaultMaxDcRatio);
        }
    }
    std::printf("T14 worst even-vs-odd H2/H3 shift over levels: %.2f dB (requirement >= 6 dB)\n", worstShift);

    // Monotonic in target.
    double prev = -1e9;
    for (double tg = -1.0; tg <= 1.0001; tg += 0.25) {
        t.setTarget(tg, 1.0);
        const double r = std::max(-60.0, std::min(60.0, measureAt(t.destination().bias, t.destination().driveDb, -12.0).ratioDb()));
        CHECK(r >= prev - 1e-6);
        prev = r;
    }
    // Amount 0 => base point regardless of target; interpolation in between.
    t.setTarget(1.0, 0.0);
    CHECK(std::fabs(t.destination().bias - 0.2) < 1e-12 && std::fabs(t.destination().driveDb) < 1e-12);
    t.setTarget(1.0, 0.5);
    CHECK(std::fabs(t.destination().bias - (0.2 + 0.5 * (even.bias - 0.2))) < 1e-9);
    // Clamping and invalid values.
    CHECK(t.setTarget(5.0, 9.0));
    CHECK(std::fabs(t.destination().bias - even.bias) < 1e-12);
    CHECK(t.setTarget(-5.0, -3.0));
    CHECK(std::fabs(t.destination().bias - 0.2) < 1e-12);
    const auto keep = t.destination();
    CHECK(!t.setTarget(kNan, 1.0) && !t.setTarget(0.0, kNan));
    CHECK(t.destination().bias == keep.bias);

    // 50 ms smoothing: 95% of the way after 50 ms, monotone, no overshoot; partition invariant.
    t.configure(&map, 0.2, 0.0, kFs);
    t.setTarget(1.0, 1.0);
    const double b0 = t.bias(), bT = t.destination().bias;
    t.advance(2400);
    const double frac = (t.bias() - b0) / (bT - b0);
    CHECK(std::fabs(frac - 0.95) < 0.005);
    HarmonicTarget a, b;
    a.configure(&map, 0.2, 0.0, kFs);
    b.configure(&map, 0.2, 0.0, kFs);
    a.setTarget(-1.0, 0.7); b.setTarget(-1.0, 0.7);
    a.advance(8192);
    for (int i = 0; i < 8192; i += 127) b.advance(static_cast<std::size_t>(std::min(127, 8192 - i)));
    CHECK(a.bias() == b.bias() && a.driveDb() == b.driveDb());
    a.advance(0);

    // Drive bound holds when the base drive sits near an edge of the map.
    HarmonicTarget edge;
    CHECK(edge.configure(&map, 0.2, -11.0, kFs));
    edge.setTarget(1.0, 1.0);
    CHECK(std::fabs(edge.destination().driveDb + 11.0) <= 6.0 + 1e-12);
    CHECK(edge.destination().driveDb >= -12.0 - 1e-12);
    // Base inside unstable (DC-excess) region: no safe branch, feature disabled with a reason.
    HarmonicTarget unsafe;
    CHECK(!unsafe.configure(&map, 0.2, 11.0, kFs) && unsafe.reason() == DisabledReason::NoMonotonicBranch);
    CHECK(!edge.configure(&map, kNan, 0.0, kFs) && edge.reason() == DisabledReason::InvalidBase);
}
}  // namespace

int main() {
    measurementHelper();
    missingMap();
    syntheticFixture();
    if (failures == 0) std::printf("control harmonic: all T14 checks passed (SYNTHETIC fixture, not calibrated)\n");
    return failures == 0 ? 0 : 1;
}
