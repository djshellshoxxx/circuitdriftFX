// T19: fault injector (CD-F19). The "circuit" is a SYNTHETIC one-pole RC + bias fixture driven by the
// overlay outputs; it exercises numerical containment only and says nothing about real failing parts.
#include "blueprint/Faults.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace {
using namespace cd::blueprint;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000;
const FaultBounds kB{};   // defaults: C 1u->1n, ESR 0.1->1M, leak 1G->100, R 10k->100M, span 1 V

// R from the input into node y; leak resistor y->gnd; cap (with ESR) y->gnd; implicit Euler (unconditionally stable).
struct RcFixture {
    double y = 0, vc = 0;
    double step(double x, const FaultOutputs& o, double dt) {
        const double R = o.resistanceOhm, Rl = o.leakageROhm, esr = o.seriesROhm, C = o.capacitanceF;
        const double G = 1.0 / R + 1.0 / Rl + 1.0 / esr;
        const double A = (x / R) / G, B = (1.0 / esr) / G, k = dt / (C * esr);
        vc = (vc + k * A) / (1.0 + k * (1.0 - B));
        y = A + B * vc;
        return y + o.biasPerturbationV;
    }
};

struct RunStats {
    bool finite = true, inBounds = true;
    double peak = 0, maxBias = 0;
    std::vector<long> toggles;
    std::uint64_t hash = 14695981039346656037ull;
    FaultOutputs last;
    double minC = 1e300, maxEsr = 0, minLeak = 1e300, maxR = 0;
};

void hashIn(std::uint64_t& h, double v) {
    std::uint64_t u;
    std::memcpy(&u, &v, sizeof u);
    h = (h ^ u) * 1099511628211ull;
}

FaultOverlay overlay(FaultType t, double sev, std::uint64_t seed = 1234) {
    FaultOverlay o;
    o.type = t;
    o.severity = sev;
    o.enabled = true;
    o.seed = seed;
    o.revision = 1;
    return o;
}

bool outputsInside(const FaultOutputs& o) {
    return o.capacitanceF >= kB.minC && o.capacitanceF <= kB.nominalC && o.seriesROhm >= kB.nominalEsr && o.seriesROhm <= kB.openSeriesR &&
           o.leakageROhm >= kB.shortLeakR && o.leakageROhm <= kB.nominalLeakR && o.resistanceOhm >= kB.nominalR && o.resistanceOhm <= kB.openR &&
           std::fabs(o.biasPerturbationV) <= kBiasPerturbationMax * kB.biasSpanV + 1e-15;
}

// drive: 0 = silence, 1 = -6 dBFS gated 440 Hz bursts (100 ms on / 400 ms off => repeated transients).
RunStats run(FaultChannel& ch, int drive, double seconds, std::vector<double>* trace = nullptr) {
    RunStats s;
    RcFixture fx;
    const long n = long(seconds * kRate);
    const double amp = std::pow(10.0, -6.0 / 20.0);
    double prevAbs = 0;
    long lastTrans = ch.contactTransitions();
    for (long i = 0; i < n; ++i) {
        double x = 0;
        if (drive == 1 && (i % 24000) < 4800) x = amp * std::sin(2 * kPi * 440.0 * double(i) / kRate);
        const double ex = std::min(1.0, std::max(0.0, (std::fabs(x) - prevAbs) * 8.0));   // crude onset detector
        prevAbs = std::fabs(x);
        const FaultOutputs o = ch.tick(ex);
        if (ch.contactTransitions() != lastTrans) { s.toggles.push_back(i); lastTrans = ch.contactTransitions(); }
        const double y = fx.step(x, o, 1.0 / kRate);
        if (!std::isfinite(y)) s.finite = false;
        if (!outputsInside(o)) s.inBounds = false;
        s.peak = std::max(s.peak, std::fabs(y));
        s.maxBias = std::max(s.maxBias, std::fabs(o.biasPerturbationV));
        s.minC = std::min(s.minC, o.capacitanceF); s.maxEsr = std::max(s.maxEsr, o.seriesROhm);
        s.minLeak = std::min(s.minLeak, o.leakageROhm); s.maxR = std::max(s.maxR, o.resistanceOhm);
        hashIn(s.hash, y);
        hashIn(s.hash, o.resistanceOhm);
        if (trace) trace->push_back(o.resistanceOhm);
        s.last = o;
    }
    return s;
}

bool prepared(FaultChannel& ch, const FaultOverlay& o, double rate = kRate) {
    std::string e;
    return ch.prepare(rate, kB, o, e);
}

bool rateLimitHolds(const std::vector<long>& t) {   // at most 20 transitions inside any 1 s window
    for (std::size_t i = 0; i + 20 < t.size(); ++i) if (t[i + 20] - t[i] < long(kRate)) return false;
    for (std::size_t i = 1; i < t.size(); ++i) if (t[i] - t[i - 1] < long(kMinContactDwellSeconds * kRate)) return false;
    return true;
}

void t19Sweep() {
    RunStats baseline;
    {   // reference: no fault at all
        FaultChannel ch;
        FaultOverlay off = overlay(FaultType::ElectrolyticOpen, 0);
        off.enabled = false;
        CHECK(prepared(ch, off));
        baseline = run(ch, 1, 5);
    }
    const FaultType types[] = {FaultType::ElectrolyticOpen, FaultType::ElectrolyticLeaky, FaultType::CrackedResistor, FaultType::MicrophonicTube};
    for (FaultType t : types)
        for (double sev : {0.0, 0.5, 1.0})
            for (int drive = 0; drive < 2; ++drive) {
                FaultChannel ch;
                CHECK(prepared(ch, overlay(t, sev)));
                const RunStats s = run(ch, drive, 60.0);      // 60 s at 48 kHz
                CHECK(s.finite && s.inBounds);
                CHECK(s.peak < 1.06);                          // |x| <= 0.5012, DC gain <= 1, bias <= 0.05
                CHECK(rateLimitHolds(s.toggles));
                CHECK(s.maxBias <= kBiasPerturbationMax * sev * kB.biasSpanV + 1e-15);
                if (sev == 0) {                                // severity 0 is exactly the nominal circuit
                    CHECK(s.minC == kB.nominalC && s.maxEsr == kB.nominalEsr && s.minLeak == kB.nominalLeakR && s.maxR == kB.nominalR);
                    CHECK(s.toggles.empty() && s.maxBias == 0);
                    if (drive == 1) {
                        FaultChannel ref;
                        FaultOverlay off = overlay(t, 0);
                        off.enabled = false;
                        CHECK(prepared(ref, off));
                        CHECK(run(ref, 1, 60.0).hash == s.hash);
                    }
                } else {
                    if (t == FaultType::ElectrolyticOpen) CHECK(s.last.capacitanceF < kB.nominalC && s.last.seriesROhm > kB.nominalEsr);
                    if (t == FaultType::ElectrolyticLeaky) CHECK(s.last.leakageROhm < kB.nominalLeakR);
                    if (t == FaultType::MicrophonicTube && drive == 1) CHECK(s.maxBias > 0);
                    if (t == FaultType::MicrophonicTube && drive == 0) CHECK(s.maxBias == 0);
                    if (t == FaultType::CrackedResistor) CHECK(s.toggles.size() > 100 && s.maxR > kB.nominalR);
                }
                if (sev == 1.0 && t == FaultType::ElectrolyticOpen) CHECK(s.last.capacitanceF == kB.minC && s.last.seriesROhm == kB.openSeriesR);
                if (sev == 1.0 && t == FaultType::ElectrolyticLeaky) CHECK(s.last.leakageROhm == kB.shortLeakR && s.last.leakageROhm > 0);
            }
    (void)baseline;
    // Same sweep at 4x oversampled electrical rate for the resonator (5 s) — bounded and finite.
    FaultChannel ch;
    FaultOverlay o = overlay(FaultType::MicrophonicTube, 1.0);
    o.frequencyHz = 4000;
    o.q = 30;
    CHECK(prepared(ch, o, 192000));
    double peak = 0;
    for (long i = 0; i < 192000 * 5; ++i) peak = std::max(peak, std::fabs(ch.tick((i % 9600) == 0 ? 1.0 : 0.0).biasPerturbationV));
    CHECK(peak > 0.04 && peak <= 0.05 + 1e-15);
}

void t19SeededRepeat() {
    auto once = [](std::uint64_t seed, std::vector<double>* tr = nullptr) {
        FaultChannel ch;
        CHECK(prepared(ch, overlay(FaultType::CrackedResistor, 0.7, seed)));
        return run(ch, 1, 20.0, tr);
    };
    const RunStats a = once(42), b = once(42), c = once(43);
    CHECK(a.toggles == b.toggles && a.hash == b.hash && !a.toggles.empty());
    CHECK(c.toggles != a.toggles && c.hash != a.hash);
    CHECK(rateLimitHolds(a.toggles) && rateLimitHolds(c.toggles));
    // Pattern is independent of when other state is touched: re-preparing the same channel reproduces it.
    FaultChannel ch;
    CHECK(prepared(ch, overlay(FaultType::CrackedResistor, 0.7, 42)));
    run(ch, 0, 1.0);
    CHECK(prepared(ch, overlay(FaultType::CrackedResistor, 0.7, 42)));
    CHECK(run(ch, 1, 20.0).hash == a.hash);
    // Cracked resistor without intermittence: steady raise, no contact toggles.
    FaultChannel st;
    FaultOverlay so = overlay(FaultType::CrackedResistor, 0.5);
    so.intermittent = false;
    CHECK(prepared(st, so));
    const RunStats s = run(st, 1, 2.0);
    CHECK(s.toggles.empty() && s.last.resistanceOhm > kB.nominalR && s.last.resistanceOhm < kB.openR);
    CHECK(std::fabs(std::log(s.last.resistanceOhm / kB.nominalR) / std::log(kB.openR / kB.nominalR) - 0.5) < 1e-9);   // log interpolation
}

void t19Transitions() {
    // C/R changes interpolate in the log domain over 10 ms (480 samples @48 kHz); never jumps.
    FaultChannel ch;
    FaultOverlay off = overlay(FaultType::ElectrolyticOpen, 0);
    off.enabled = false;
    CHECK(prepared(ch, off));
    for (int i = 0; i < 100; ++i) ch.tick(0);
    FaultOverlay on = overlay(FaultType::ElectrolyticOpen, 1.0);
    on.revision = 2;
    CHECK(ch.apply(on));
    std::vector<FaultOutputs> o;
    for (int i = 0; i < 600; ++i) o.push_back(ch.tick(0));
    CHECK(o[0].capacitanceF < kB.nominalC && o[0].capacitanceF > kB.nominalC * 0.9);   // first step is small
    const double mid = std::exp(std::log(kB.nominalC) + (std::log(kB.minC) - std::log(kB.nominalC)) * (240.0 / 480.0));
    CHECK(std::fabs(o[239].capacitanceF / mid - 1) < 1e-9);
    CHECK(o[479].capacitanceF == kB.minC && o[479].seriesROhm == kB.openSeriesR);
    for (std::size_t i = 1; i < o.size(); ++i) CHECK(o[i].capacitanceF <= o[i - 1].capacitanceF);
    // Cracked-resistor contact transitions: 2 ms (96 samples) smooth ramps.
    FaultChannel cr;
    CHECK(prepared(cr, overlay(FaultType::CrackedResistor, 1.0, 9)));
    std::vector<double> tr;
    RunStats s = run(cr, 0, 5.0, &tr);
    CHECK(s.toggles.size() > 5);
    const long t0 = s.toggles[1];
    // R(t) after a toggle: log-linear over 96 samples and then flat until the next toggle.
    const double r0 = tr[std::size_t(t0 - 1)], r1 = tr[std::size_t(t0 + 120)];
    CHECK(r0 != r1);
    for (long k = 0; k < 95; ++k) CHECK(std::fabs(tr[std::size_t(t0 + k + 1)] - tr[std::size_t(t0 + k)]) < std::fabs(r1 - r0) * 0.5);
    CHECK(tr[std::size_t(t0 + 95)] == tr[std::size_t(t0 + 96)] || std::fabs(tr[std::size_t(t0 + 95)] - r1) <= std::fabs(r1 - r0) * 0.5);
    CHECK(tr[std::size_t(t0 + 96)] == r1);                                  // exactly landed
    CHECK(rateLimitHolds(s.toggles));
}

void t19Reset() {
    const FaultType types[] = {FaultType::ElectrolyticOpen, FaultType::ElectrolyticLeaky, FaultType::CrackedResistor, FaultType::MicrophonicTube};
    for (FaultType t : types) {
        FaultChannel ch;
        CHECK(prepared(ch, overlay(t, 1.0, 5)));
        run(ch, 1, 1.5);
        CHECK(!ch.cleared());
        ch.reset();
        FaultOutputs prev;
        RcFixture fx;
        for (int i = 0; i < 960; ++i) {           // 20 ms ramp
            const FaultOutputs o = ch.tick(i < 10 ? 1.0 : 0.0);
            CHECK(outputsInside(o) && std::isfinite(fx.step(0.5, o, 1.0 / kRate)));
            prev = o;
        }
        CHECK(ch.cleared() && !ch.overlay().enabled && ch.overlay().severity == 0);
        CHECK(prev.capacitanceF == kB.nominalC && prev.seriesROhm == kB.nominalEsr && prev.leakageROhm == kB.nominalLeakR &&
              prev.resistanceOhm == kB.nominalR && prev.biasPerturbationV == 0 && !prev.contactOpen);
        const long tr0 = ch.contactTransitions();
        for (int i = 0; i < 48000 * 3; ++i) {    // no stale overlay afterwards, even under hard excitation
            const FaultOutputs o = ch.tick(1.0);
            CHECK(o.capacitanceF == kB.nominalC && o.resistanceOhm == kB.nominalR && o.biasPerturbationV == 0 && o.leakageROhm == kB.nominalLeakR);
        }
        CHECK(ch.contactTransitions() == tr0);
        ch.reset();                               // idempotent on a cleared channel
        CHECK(ch.tick(1.0).resistanceOhm == kB.nominalR);
        FaultOverlay again = overlay(t, 0.5, 5);
        again.revision = 5;
        CHECK(ch.apply(again));                   // channel is reusable after a reset
    }
    // Ramp is gradual (20 ms) and monotone for the capacitor.
    FaultChannel ch;
    CHECK(prepared(ch, overlay(FaultType::ElectrolyticOpen, 1.0)));
    for (int i = 0; i < 2000; ++i) ch.tick(0);
    ch.reset();
    double last = ch.tick(0).capacitanceF;
    CHECK(last < kB.nominalC && last > kB.minC);
    for (int i = 1; i < 960; ++i) { const double c = ch.tick(0).capacitanceF; CHECK(c >= last); last = c; }
    // FaultBank clears every channel.
    FaultBank bank;
    for (int i = 0; i < 3; ++i) { CHECK(prepared(bank.channel(i), overlay(FaultType(i), 1.0))); }
    bank.resetAll();
    for (int i = 0; i < 960; ++i) for (int k = 0; k < 3; ++k) bank.channel(k).tick(0);
    for (int k = 0; k < 3; ++k) CHECK(bank.channel(k).cleared());
}

void t19Microphonic() {
    // Independent re-implementation of z[n] = 2 r cos(2 pi f/Fs) z[n-1] - r^2 z[n-2] + g x[n].
    FaultOverlay o = overlay(FaultType::MicrophonicTube, 1.0);
    o.frequencyHz = 1200;
    o.q = 12;
    FaultChannel ch;
    CHECK(prepared(ch, o));
    const double r = std::exp(-kPi * o.frequencyHz / (o.q * kRate)), c1 = 2 * r * std::cos(2 * kPi * o.frequencyHz / kRate), c2 = r * r;
    double z1 = 0, z2 = 0, peak = 0;
    for (int i = 0; i < 20000; ++i) { const double z = c1 * z1 - c2 * z2 + (i == 0 ? 1 : 0); z2 = z1; z1 = z; peak = std::max(peak, std::fabs(z)); }
    CHECK(std::fabs(ch.resonatorGain() * peak - 1.0) < 1e-12);            // g normalised by the measured peak impulse response
    const double g = 1.0 / peak;
    std::mt19937_64 rng(7);
    z1 = z2 = 0;
    double maxErr = 0;
    for (int i = 0; i < 5000; ++i) {
        const double x = (rng() % 100) * 1e-4;                             // small, so no clamp engages
        const double z = c1 * z1 - c2 * z2 + g * x;
        z2 = z1; z1 = z;
        maxErr = std::max(maxErr, std::fabs(ch.tick(x).biasPerturbationV - kBiasPerturbationMax * z));
    }
    CHECK(maxErr < 1e-12);
    // Unit impulse peaks at exactly the normalised amplitude (5% of the safe span at severity 1), never above.
    FaultChannel imp;
    CHECK(prepared(imp, o));
    double ip = 0;
    for (int i = 0; i < 20000; ++i) ip = std::max(ip, std::fabs(imp.tick(i == 0 ? 1.0 : 0.0).biasPerturbationV));
    CHECK(std::fabs(ip - 0.05) < 1e-9);
    // Hostile excitation (sustained, NaN, inf, negative, huge) stays inside 5% of the span.
    FaultChannel bad;
    CHECK(prepared(bad, o));
    double bp = 0;
    const double hostile[] = {1.0, 1e300, std::nan(""), std::numeric_limits<double>::infinity(), -5.0, 0.0};
    for (int i = 0; i < 48000 * 10; ++i) {
        const double v = bad.tick(hostile[(i / 4800) % 6]).biasPerturbationV;
        CHECK(std::isfinite(v));
        bp = std::max(bp, std::fabs(v));
    }
    CHECK(bp <= 0.05 + 1e-15);
    // Severity scales the bound: 0.4 -> 2%.
    FaultChannel half;
    FaultOverlay h = o;
    h.severity = 0.4;
    CHECK(prepared(half, h));
    double hp = 0;
    for (int i = 0; i < 48000; ++i) hp = std::max(hp, std::fabs(half.tick(1.0).biasPerturbationV));
    CHECK(hp <= 0.02 + 1e-15 && hp > 0.019);
    // Parameter ranges: f 100..4000, Q 2..30.
    FaultChannel pr;
    std::string e;
    for (double f : {99.0, 4001.0, -1.0, std::nan("")}) { FaultOverlay x = o; x.frequencyHz = f; CHECK(!pr.prepare(kRate, kB, x, e)); }
    for (double q : {1.9, 30.1, 0.0, std::nan("")}) { FaultOverlay x = o; x.q = q; CHECK(!pr.prepare(kRate, kB, x, e)); }
    for (double f : {100.0, 800.0, 4000.0}) for (double q : {2.0, 30.0}) { FaultOverlay x = o; x.frequencyHz = f; x.q = q; CHECK(pr.prepare(kRate, kB, x, e)); }
}

void t19Validation() {
    FaultChannel ch;
    CHECK(prepared(ch, overlay(FaultType::CrackedResistor, 0.5)));
    FaultOverlay bad = overlay(FaultType::CrackedResistor, 0.5);
    bad.revision = 2;
    for (double s : {-0.1, 1.1, std::nan(""), std::numeric_limits<double>::infinity()}) { bad.severity = s; CHECK(!ch.apply(bad)); }
    bad.severity = 0.5;
    bad.frequencyHz = 5000;
    CHECK(!ch.apply(bad));
    bad.frequencyHz = 800;
    bad.q = 100;
    CHECK(!ch.apply(bad));
    bad.q = 8;
    bad.type = FaultType(9);
    CHECK(!ch.apply(bad));
    bad.type = FaultType::CrackedResistor;
    CHECK(ch.apply(bad));
    FaultOverlay stale = bad;
    stale.revision = 1;
    stale.severity = 0.1;
    CHECK(!ch.apply(stale) && ch.overlay().severity == 0.5);              // stale revision ignored
    FaultOverlay retune = bad;
    retune.revision = 3;
    retune.frequencyHz = 900;
    CHECK(!ch.apply(retune));                                             // retuning needs prepare() (g re-measured)
    // Unprepared channel is inert; sad bounds are rejected (never ideal 0/inf).
    FaultChannel un;
    const FaultOutputs z = un.tick(1.0);
    CHECK(z.resistanceOhm == 0 && z.biasPerturbationV == 0 && !un.apply(overlay(FaultType::ElectrolyticOpen, 1)));
    std::string e;
    FaultBounds b = kB;
    b.minC = 0; CHECK(!validBounds(b) && !ch.prepare(kRate, b, overlay(FaultType::ElectrolyticOpen, 1), e));
    b = kB; b.openR = std::numeric_limits<double>::infinity(); CHECK(!validBounds(b));
    b = kB; b.shortLeakR = 0; CHECK(!validBounds(b));
    b = kB; b.openSeriesR = std::nan(""); CHECK(!validBounds(b));
    b = kB; b.minC = 1; CHECK(!validBounds(b));                           // minimum above nominal
    CHECK(!ch.prepare(100, kB, overlay(FaultType::ElectrolyticOpen, 1), e) && !ch.prepare(std::nan(""), kB, overlay(FaultType::ElectrolyticOpen, 1), e));
    // Availability with reasons.
    const char* why = nullptr;
    CHECK(faultAvailable(FaultType::ElectrolyticOpen, true, false, false, why) && why[0] == 0);
    CHECK(!faultAvailable(FaultType::ElectrolyticLeaky, false, true, false, why) && why[0] != 0);
    CHECK(faultAvailable(FaultType::CrackedResistor, false, true, false, why));
    CHECK(!faultAvailable(FaultType::MicrophonicTube, false, false, false, why) && std::strstr(why, "G01"));
}

}  // namespace

int main() {
    t19Sweep();
    t19SeededRepeat();
    t19Transitions();
    t19Reset();
    t19Microphonic();
    t19Validation();
    if (failures) std::printf("blueprint_faults: %d FAILED\n", failures);
    else std::printf("blueprint_faults: all checks passed\n");
    return failures ? 1 : 0;
}
