// T13 (CD-F13) tempo clock checks. Pure arithmetic/synthetic; no hardware data.
#include "control/TempoClock.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace {
using namespace cd::control;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)
const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

HostTempo host(double bpm, double ppq, bool ppqOk = true) {
    HostTempo h;
    h.bpm = bpm; h.bpmValid = true; h.ppq = ppq; h.ppqValid = ppqOk;
    h.timeSigNum = 4; h.timeSigDen = 4; h.timeSigValid = true;
    return h;
}

// One-pole driven by the clock coefficient; time to reach 95% of a unit step (linear interpolation).
double recoverySeconds(const TempoClock& c, double fs) {
    const double a = c.recoveryCoefficient();
    double y = 0.0;
    for (long n = 1; n < static_cast<long>(20 * fs); ++n) {
        const double prev = y;
        y += a * (1.0 - y);
        if (y >= 0.95) return (static_cast<double>(n - 1) + (0.95 - prev) / (y - prev)) / fs;
    }
    return -1.0;
}

void beatMath() {
    CHECK(TempoClock::beats(Division::Quarter, Modifier::Straight, 4, 4) == 1.0);
    CHECK(TempoClock::beats(Division::ThirtySecond, Modifier::Straight, 4, 4) == 0.125);
    CHECK(TempoClock::beats(Division::Eighth, Modifier::Dotted, 4, 4) == 0.75);
    CHECK(std::fabs(TempoClock::beats(Division::Eighth, Modifier::Triplet, 4, 4) - 1.0 / 3.0) < 1e-15);
    CHECK(TempoClock::beats(Division::Bar, Modifier::Straight, 4, 4) == 4.0);
    CHECK(TempoClock::beats(Division::Bar, Modifier::Straight, 7, 8) == 3.5);
    CHECK(TempoClock::beats(Division::Bar, Modifier::Straight, 6, 8) == 3.0);
    CHECK(TempoClock::beats(Division::Bar, Modifier::Straight, 0, 4) == 4.0);  // bad signature -> 4/4
    CHECK(std::fabs(TempoClock::durationSeconds(1.0, 120.0) - 0.5) < 1e-15);
    CHECK(TempoClock::coefficientFor(0.0, 48000.0) == 0.0);
    CHECK(TempoClock::coefficientFor(kNan, 48000.0) == 0.0);
}

void recoveryTime() {
    const double fs = 48000.0;
    double worst = 0.0;
    for (const double bpm : {60.0, 120.0, 180.0}) {
        for (const Division d : {Division::Sixteenth, Division::Eighth, Division::Quarter, Division::Half, Division::Bar}) {
            for (const Modifier m : {Modifier::Straight, Modifier::Dotted, Modifier::Triplet}) {
                TempoClock c;
                CHECK(c.prepare(fs));
                c.setDivision(d, m);
                c.setHost(host(bpm, 0.0));
                c.advance(2401);  // let the 50 ms coefficient smoothing finish
                const double D = c.durationSeconds();
                const double meas = recoverySeconds(c, fs);
                const double err = std::fabs(meas - D) / D;
                worst = std::max(worst, err);
                CHECK(err < 0.01);
            }
        }
    }
    std::printf("T13 worst 95%% recovery error vs D: %.6f%% (limit 1%%)\n", worst * 100.0);
}

void fallbackVisible() {
    TempoClock c;
    CHECK(c.prepare(48000.0));
    HostTempo h;  // nothing valid
    c.setHost(h);
    CHECK(c.status().bpmFallback && c.status().ppqFreeRun && c.status().timeSigFallback);
    CHECK(c.bpm() == 120.0);
    HostTempo nan = host(kNan, 0.0);
    c.setHost(nan);
    CHECK(c.status().bpmFallback && c.bpm() == 120.0);
    c.setHost(host(kInf, 0.0));
    CHECK(c.status().bpmFallback);
    c.setHost(host(-5.0, 0.0));
    CHECK(c.status().bpmFallback);
    c.setHost(host(1000.0, 0.0));
    CHECK(!c.status().bpmFallback && c.status().bpmClamped && c.bpm() == 300.0);
    c.setHost(host(5.0, 0.0));
    CHECK(c.status().bpmClamped && c.bpm() == 20.0);
    c.setHost(host(100.0, 0.0));
    CHECK(!c.status().bpmFallback && !c.status().bpmClamped && !c.status().ppqFreeRun);
    c.setHost(host(100.0, kNan));
    CHECK(c.status().ppqFreeRun);
    CHECK(!c.prepare(0.0) && !c.prepare(kNan));
    TempoClock u;  // unprepared: no crash, no change
    u.setHost(host(120.0, 0.0));
    u.step();
    CHECK(u.recoveryCoefficient() == 0.0);
}

void phaseContinuity() {
    const double fs = 48000.0;
    TempoClock c;
    c.prepare(fs);
    c.setDivision(Division::Quarter, Modifier::Straight);
    c.setHost(host(120.0, 0.0));
    c.advance(48000);  // 1 s at 120 BPM = 2 beats
    CHECK(std::fabs(c.ppq() - 2.0) < 1e-9);
    const double before = c.phase();
    c.setHost(host(90.0, 2.0));  // tempo change: ppq follows sample clock, no jump
    CHECK(std::fabs(c.phase() - before) < 1e-9);
    double maxStep = 0.0, last = c.phase();
    for (int i = 0; i < 48000; ++i) {
        c.step();
        double d = std::fabs(c.phase() - last);
        if (d > 0.5) d = 1.0 - d;
        maxStep = std::max(maxStep, d);
        last = c.phase();
    }
    CHECK(maxStep < 90.0 / 60.0 / fs * 1.01);
    // Recovery coefficient is smoothed over 50 ms after the tempo change.
    TempoClock s;
    s.prepare(fs);
    s.setHost(host(60.0, 0.0));
    const double a60 = s.recoveryCoefficient();
    s.setHost(host(180.0, 0.0));
    CHECK(s.recoveryCoefficient() == a60);  // ramp starts, nothing jumps
    CHECK(s.targetCoefficient() > a60);
    s.advance(1200);  // 25 ms: halfway
    const double mid = s.recoveryCoefficient();
    CHECK(mid > a60 && mid < s.targetCoefficient());
    s.advance(1200);
    CHECK(s.recoveryCoefficient() == s.targetCoefficient());
}

void seekAndFreeRun() {
    const double fs = 48000.0;
    TempoClock c;
    c.prepare(fs);
    c.setDivision(Division::Quarter, Modifier::Straight);
    c.setHost(host(120.0, 10.25));
    CHECK(std::fabs(c.phase() - 0.25) < 1e-12);  // adopts PPQ
    c.advance(4800);  // 0.1 s = 0.2 beats
    c.setHost(host(120.0, 10.45));  // consistent: no reset
    CHECK(std::fabs(c.ppq() - 10.45) < 1e-9);
    HostTempo loop = host(120.0, 2.0);
    c.setHost(loop);  // large discrepancy => seek
    CHECK(std::fabs(c.ppq() - 2.0) < 1e-12);
    loop = host(120.0, 2.01);
    loop.transportJump = true;
    c.setHost(loop);
    CHECK(c.ppq() == 2.01);
    // Free-run without PPQ.
    HostTempo fr = host(120.0, 0.0, false);
    c.setHost(fr);
    const double p0 = c.ppq();
    c.advance(48000);
    CHECK(std::fabs(c.ppq() - p0 - 2.0) < 1e-9);
    CHECK(c.status().ppqFreeRun);
    // Negative ppq phase stays in [0,1).
    c.setHost(host(120.0, -0.25));
    CHECK(c.phase() >= 0.0 && c.phase() < 1.0 && std::fabs(c.phase() - 0.75) < 1e-12);
    // Invalid host ppq ignored.
    c.setHost(host(120.0, kInf));
    CHECK(c.status().ppqFreeRun);
    CHECK(std::fabs(c.drift(2.0)) <= 1.0 && c.drift(kNan) == 0.0);
}

void partitionInvariance() {
    const double fs = 48000.0;
    const long total = 100000;
    const int parts[] = {1, 17, 64, 127, 512, 8192};
    double refPhase = 0, refCoef = 0;
    bool first = true;
    for (const int blk : parts) {
        TempoClock c;
        c.prepare(fs);
        c.setDivision(Division::Eighth, Modifier::Dotted);
        double bpm = 120.0;
        long done = 0;
        double truePpq = 0.0;
        while (done < total) {
            const long n = std::min<long>(blk, total - done);
            if (done >= 50000) bpm = 133.0;  // tempo change at a fixed sample (aligned to block edge below)
            // The change must land at the same sample for every partition: split blocks at 50000.
            const long lim = done < 50000 ? std::min<long>(n, 50000 - done) : n;
            c.setHost(host(bpm, truePpq));
            c.advance(static_cast<std::size_t>(lim));
            truePpq += static_cast<double>(lim) * bpm / (60.0 * fs);
            done += lim;
        }
        if (first) { refPhase = c.phase(); refCoef = c.recoveryCoefficient(); first = false; }
        CHECK(std::fabs(c.phase() - refPhase) < 1e-7);
        CHECK(std::fabs(c.recoveryCoefficient() - refCoef) < 1e-15);
    }
    TempoClock z;
    z.prepare(fs);
    z.advance(0);  // zero-frame block
    CHECK(z.ppq() == 0.0);
}
}  // namespace

int main() {
    beatMath();
    recoveryTime();
    fallbackVisible();
    phaseContinuity();
    seekAndFreeRun();
    partitionInvariance();
    if (failures == 0) std::printf("control tempo: all T13 checks passed\n");
    return failures == 0 ? 0 : 1;
}
