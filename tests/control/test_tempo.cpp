// T13: tempo breathing (control-level): first-order recovery timing, phase continuity, fallback flags.
#include <algorithm>
#include <cmath>
#include <vector>
#include "TestHarness.h"
#include "control/ControlProcessor.h"
#include "control/TempoBreathing.h"

using namespace cd;
using namespace cd::control;

static TempoOutput settle(TempoBreathing& t, TempoInput in, int blocks = 200, int frames = 480) {
    TempoOutput o;
    for (int i = 0; i < blocks; ++i) o = t.process(in, frames);
    return o;
}

static void recoveryTiming() {
    constexpr double fs = 48000.0;
    const double divQ[] = {0.125, 0.25, 0.5, 1.0, 2.0};
    for (double bpm : {60.0, 120.0, 180.0}) {
        for (int div = 0; div <= 5; ++div) {
            for (int mod = 0; mod < 3; ++mod) {
                TempoBreathing t;
                t.prepare(fs);
                TempoInput in;
                in.enabled = true;
                in.division = div;
                in.modifier = mod;
                in.bpm = bpm;
                const TempoOutput o = settle(t, in, 100);
                double q = div < 5 ? divQ[div] : 4.0;  // default 4/4 bar
                if (mod == 1) q *= 1.5;
                if (mod == 2) q *= 2.0 / 3.0;
                const double D = 60.0 * q / bpm;
                CHECK(std::fabs(o.beatsPerCycle - q) < 1e-12, "q");
                // Isolated first-order response 1-exp(-t/tau): 95% reached at t95.
                const double tau = o.recoveryTauS;
                const double t95 = -tau * std::log(1.0 - 0.95);
                CHECK(std::fabs(t95 - D) / D < 0.01, "bpm %g div %d mod %d: t95 %g vs D %g", bpm, div, mod, t95, D);
                // Time-domain: simulate the recovery with fine steps and find the 95% crossing.
                const double dtSim = D / 20000.0;
                double v = 0, tt = 0;
                while (v < 0.95 && tt < 10 * D) { v += (1.0 - v) * (1.0 - std::exp(-dtSim / tau)); tt += dtSim; }
                CHECK(std::fabs(tt - D) / D < 0.01, "simulated t95 %g vs %g", tt, D);
            }
        }
    }
    // Bars use numerator*4/denominator: 3/4 -> 3 quarters, 6/8 -> 3, 7/8 -> 3.5.
    TempoBreathing t;
    t.prepare(fs);
    TempoInput in;
    in.enabled = true;
    in.division = 5;
    in.bpm = 120.0;
    in.timeSigNum = 7; in.timeSigDen = 8;
    CHECK(std::fabs(settle(t, in, 10).beatsPerCycle - 3.5) < 1e-12, "7/8 bar");
    in.timeSigNum = 3; in.timeSigDen = 4;
    CHECK(std::fabs(settle(t, in, 10).beatsPerCycle - 3.0) < 1e-12, "3/4 bar");
    // BPM clamp 20..300.
    CHECK(TempoBreathing::clampBpm(5) == 20 && TempoBreathing::clampBpm(900) == 300 && TempoBreathing::clampBpm(NAN) == 120, "clamp");
    in.bpm = 5.0;
    CHECK(settle(t, in, 10).bpmUsed == 20.0, "bpm clamped low");
    in.bpm = 1000.0;
    CHECK(settle(t, in, 10).bpmUsed == 300.0, "bpm clamped high");
}

static void fallbackAndDisabled() {
    TempoBreathing t;
    t.prepare(48000.0);
    TempoInput in;
    in.enabled = true;
    in.division = 3;
    TempoOutput o = settle(t, in, 10);  // no bpm, no ppq
    CHECK(o.fallback && o.bpmUsed == 120.0, "missing BPM -> 120 + fallback flag");
    CHECK(std::fabs(o.recoveryTauS - 0.5 / std::log(20.0)) < 1e-9, "tau at fallback tempo");
    in.bpm = 90.0;
    o = settle(t, in, 10);
    CHECK(!o.fallback, "fallback cleared with host BPM");
    in.enabled = false;
    o = settle(t, in, 10);
    CHECK(o.recoveryTauS == 0 && o.cutoffOct == 0 && !o.fallback, "disabled -> natural recovery, no flag");
    // Enabling snaps tau to its target (no ramp from a stale value).
    in.enabled = true;
    in.bpm = 60.0;
    o = t.process(in, 1);
    CHECK(std::fabs(o.recoveryTauS - 1.0 / std::log(20.0)) < 1e-3, "snap on enable: %g", o.recoveryTauS);
}

static void tauSmoothing() {
    TempoBreathing t;
    t.prepare(48000.0);
    TempoInput in;
    in.enabled = true;
    in.division = 3;
    in.bpm = 120.0;
    const double t120 = settle(t, in, 50).recoveryTauS;
    in.bpm = 60.0;
    const double t60 = 2.0 * t120;
    TempoOutput o;
    for (int i = 0; i < 5; ++i) o = t.process(in, 480);  // 50 ms
    const double frac = (o.recoveryTauS - t120) / (t60 - t120);
    CHECK(std::fabs(frac - (1 - std::exp(-1.0))) < 1e-6, "50 ms time constant: %g", frac);
}

static void phaseContinuity() {
    constexpr double fs = 48000.0;
    // Free-run: BPM 120 -> 150 mid-stream; per-block phase increments follow the sample clock.
    {
        TempoBreathing t;
        t.prepare(fs);
        TempoInput in;
        in.enabled = true;
        in.division = 3;   // q=1
        in.depth = 1.0;
        double prev = -1, maxJump = 0;
        for (int b = 0; b < 400; ++b) {
            in.bpm = b < 200 ? 120.0 : 150.0;
            const TempoOutput o = t.process(in, 480);
            const double bps = *in.bpm / 60.0;
            const double expectInc = 480.0 / fs * bps / 1.0;
            if (prev >= 0) {
                double inc = o.phase - prev;
                inc -= std::floor(inc);
                maxJump = std::max(maxJump, std::fabs(inc - expectInc));
            }
            prev = o.phase;
        }
        CHECK(maxJump < 1e-9, "free-run phase continuous across tempo change: %g", maxJump);
    }
    // PPQ-locked: host integrates ppq across a tempo change; phase = frac(ppq/q) with no jump.
    {
        TempoBreathing t;
        t.prepare(fs);
        TempoInput in;
        in.enabled = true;
        in.division = 2;   // 1/8 -> q=0.5
        in.depth = 1.0;
        double ppq = 3.0;
        double prev = -1, maxDev = 0;
        for (int b = 0; b < 400; ++b) {
            const double bpm = b < 200 ? 100.0 : 160.0;
            in.bpm = bpm;
            in.ppq = ppq;
            const TempoOutput o = t.process(in, 480);
            ppq += 480.0 / fs * bpm / 60.0;
            const double expect = std::fmod(ppq / 0.5, 1.0);
            if (prev >= 0) maxDev = std::max(maxDev, std::fabs(std::remainder(o.phase - expect, 1.0)));
            prev = o.phase;
        }
        CHECK(maxDev < 1e-9, "ppq phase tracks host ppq through tempo change: %g", maxDev);
    }
    // Seek: ppq jumps by a non-multiple of q; output phase glides to the new PPQ phase over ~20 ms.
    {
        TempoBreathing t;
        t.prepare(fs);
        TempoInput in;
        in.enabled = true;
        in.division = 3;
        in.depth = 1.0;
        in.bpm = 120.0;
        double ppq = 0.0;
        TempoOutput o;
        for (int b = 0; b < 100; ++b) { in.ppq = ppq; o = t.process(in, 480); ppq += 0.01 * 2.0; }
        const double before = o.phase;
        ppq += 0.37;  // seek (not a multiple of q)
        in.ppq = ppq;
        o = t.process(in, 48);  // 1 ms after the seek
        const double target = std::fmod(ppq + 48.0 / fs * 2.0, 1.0);
        const double hop = std::fabs(std::remainder(o.phase - before, 1.0));
        CHECK(hop < 0.08, "no jump in phase right after seek: moved %g (raw jump 0.37)", hop);
        for (int b = 0; b < 20; ++b) { in.ppq = ppq + 48.0 / fs * 2.0 + 0.01 * 2.0 * b; o = t.process(in, 480); }
        const double tgt2 = std::fmod(in.ppq.value() + 480.0 / fs * 2.0, 1.0);
        CHECK(std::fabs(std::remainder(o.phase - tgt2, 1.0)) < 1e-3, "settles onto PPQ phase after 200 ms");
        (void)target;
    }
    // Sinusoidal contribution bounded by depth*0.15 octaves; depth 0 -> 0.
    {
        TempoBreathing t;
        t.prepare(fs);
        TempoInput in;
        in.enabled = true;
        in.bpm = 120;
        in.depth = 0.5;
        double mx = 0;
        for (int b = 0; b < 400; ++b) mx = std::max(mx, std::fabs(t.process(in, 480).cutoffOct));
        CHECK(mx <= 0.075 + 1e-12 && mx > 0.07, "depth 0.5 amplitude %g", mx);
        in.depth = 0;
        CHECK(t.process(in, 480).cutoffOct == 0.0, "depth 0");
    }
}

static void processorIntegration() {
    ControlProcessor cp;
    cp.prepare(48000.0, 480, 7);
    core::ParamValues p = core::defaultValues();
    p[core::indexOf("tempo.enabled")] = 1;
    p[core::indexOf("tempo.division")] = 3;
    core::ModulationBlock mb;
    ControlContext ctx;
    ctx.frames = 480;
    for (int i = 0; i < 100; ++i) cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    CHECK(cp.status().tempoFallback && cp.status().tempoActive, "status: fallback visible");
    CHECK(std::fabs(mb.slot[0].recoveryTauS - 0.5 / std::log(20.0)) < 1e-6 && mb.slot[1].recoveryTauS == mb.slot[0].recoveryTauS,
          "recoveryTauS in both slots");
    ctx.bpm = 120.0;
    ctx.ppq = 0.0;
    cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    CHECK(!cp.status().tempoFallback, "fallback cleared when host provides BPM");
    p[core::indexOf("tempo.enabled")] = 0;
    cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    CHECK(mb.slot[0].recoveryTauS == 0.0, "tempo off -> natural recovery");
}

int main() {
    recoveryTiming();
    fallbackAndDisabled();
    tauSmoothing();
    phaseContinuity();
    processorIntegration();
    if (g_fail) { std::printf("tempo: %d failure(s)\n", g_fail); return 1; }
    std::printf("tempo: all passed\n");
    return 0;
}
