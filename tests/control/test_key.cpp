// T12 (key/scale part): scale quantization, tie/hysteresis determinism, MIDI source timing, scale lock.
#include <algorithm>
#include <cmath>
#include <vector>
#include "TestHarness.h"
#include "control/ControlProcessor.h"
#include "control/KeyTracker.h"
#include "control/ScaleQuantizer.h"

using namespace cd;
using namespace cd::control;

static int bruteNearest(double p, uint16_t mask, int root) {
    int best = -1000;
    double bd = 1e9;
    for (int n = -20; n < 160; ++n) {
        if (!scale::isAllowed(mask, root, n)) continue;
        const double d = std::fabs(p - n);
        if (d < bd) { bd = d; best = n; }  // ascending: ties keep the lower note
    }
    return best;
}

static void scalesSettle() {
    const uint16_t custom = 0b101001010101;  // arbitrary nonzero custom mask
    th::Lcg rng;
    for (int sc = 0; sc <= 5; ++sc) {
        const uint16_t mask = scale::maskFor(sc, custom);
        CHECK(mask != 0, "scale %d mask", sc);
        for (int root = 0; root < 12; ++root) {
            for (int trial = 0; trial < 20; ++trial) {
                const double p = 24.0 + 90.0 * rng.next();  // MIDI float target
                ResonanceQuantizer q;
                q.configure(mask, root);
                double g = 0;
                for (int i = 0; i < 100; ++i) g = q.update(p, 0.01);  // 1 s >> 50 ms glide
                const int expect = bruteNearest(p, mask, root);
                CHECK(q.note() == expect, "scale %d root %d p %.3f note %d expect %d", sc, root, p, q.note(), expect);
                const double fTarget = scale::midiToHz(double(expect));
                const double fGot = scale::midiToHz(g);
                CHECK(std::fabs(th::cents(fGot, fTarget)) < 0.01, "cents err %g", th::cents(fGot, fTarget));
                CHECK(scale::isAllowed(mask, root, q.note()), "note in scale");
            }
        }
    }
    // Interval tables (spec).
    CHECK(scale::maskFor(1, 0) == 0b101010110101, "major mask");   // {0,2,4,5,7,9,11}
    CHECK(scale::maskFor(2, 0) == 0b010110101101, "minor mask");   // {0,2,3,5,7,8,10}
    CHECK(scale::maskFor(3, 0) == 0b001010010101, "pent maj");     // {0,2,4,7,9}
    CHECK(scale::maskFor(4, 0) == 0b010010101001, "pent min");     // {0,3,5,7,10}
    CHECK(scale::maskFor(5, 0) == scale::kChromatic, "zero custom mask -> chromatic");
    // Glide: 50 ms linear in log-frequency, monotone, reaches target.
    ResonanceQuantizer q;
    q.configure(scale::kChromatic, 0);
    q.update(60.0, 0.0);
    double last = q.glided();
    CHECK(last == 60.0, "first note no glide");
    q.update(72.0, 0.025);
    CHECK(std::fabs(q.glided() - 66.0) < 1e-9, "halfway at 25 ms: %g", q.glided());
    q.update(72.0, 0.025);
    CHECK(q.glided() == 72.0, "done at 50 ms");
}

static void tiesAndHysteresis() {
    // Tie goes lower: C major, A(69) and B(71) allowed, 70 not -> p=70 exactly is a tie.
    CHECK(scale::nearestAllowed(70.0, scale::maskFor(1, 0), 0) == 69, "tie -> lower (scale)");
    CHECK(scale::nearestAllowed(60.5, scale::kChromatic, 0) == 60, "tie -> lower (chromatic)");
    CHECK(scale::nearestAllowed(-0.5, scale::kChromatic, 0) == -1, "negative tie lower");
    // Hysteresis: chromatic; start at 69; switching to 70 needs p > 69.65; back needs p < 69.35.
    auto run = [](const std::vector<double>& path) {
        ResonanceQuantizer q;
        q.configure(scale::kChromatic, 0);
        std::vector<int> notes;
        for (double p : path) notes.push_back(q.decide(p));
        return notes;
    };
    const std::vector<double> path = {69.0, 69.4, 69.6, 69.64, 69.66, 69.5, 69.4, 69.36, 69.34, 69.0, 69.5, 69.5};
    const std::vector<int> expect = {69, 69, 69, 69, 70, 70, 70, 70, 69, 69, 69, 69};
    CHECK(run(path) == expect, "hysteresis walk");
    CHECK(run(path) == run(path), "deterministic");
    // Retained note drops out of hysteresis memory when the scale no longer allows it.
    ResonanceQuantizer q;
    q.configure(scale::kChromatic, 0);
    q.decide(70.0);
    q.configure(scale::maskFor(1, 0), 0);  // Bb not in C major
    CHECK(q.decide(70.0) == 69, "reconfigured scale re-decides (tie lower)");
}

static MidiEvent ev(uint32_t f, uint8_t st, uint8_t d1, uint8_t d2) { MidiEvent e; e.frame = f; e.status = st; e.data1 = d1; e.data2 = d2; return e; }

static void advanceSecs(KeyTracker& k, double secs, double fs = 48000.0) {
    k.advanceMidi({}, int(std::lround(secs * fs)));
}

static void midiTimings() {
    constexpr double fs = 48000.0;
    {  // note on/off hold 500 ms then 100 ms return
        KeyTracker k;
        k.prepare(fs);
        MidiEvent on = ev(10, 0x90, 60, 100), off = ev(100, 0x80, 60, 0);
        const MidiEvent evs[2] = {on, off};
        k.advanceMidi(evs, 200);
        CHECK(k.state() == KeyState::NoteHold && k.notePitch() == 60.0 && k.weight() == 1.0, "hold after note-off");
        advanceSecs(k, 0.49);
        CHECK(k.state() == KeyState::NoteHold && k.weight() == 1.0, "still holding at ~0.49 s");
        advanceSecs(k, 0.02);
        CHECK(k.state() == KeyState::Returning && k.weight() < 1.0 && k.weight() > 0.5, "returning after 0.5 s");
        advanceSecs(k, 0.05);
        CHECK(std::fabs(k.weight() - 0.4) < 0.15, "mid-return weight %g", k.weight());
        advanceSecs(k, 0.1);
        CHECK(k.state() == KeyState::ManualReference && k.weight() == 0.0, "manual after 100 ms ramp");
    }
    {  // last-held wins, falls back to earlier held note
        KeyTracker k;
        k.prepare(fs);
        const MidiEvent a[2] = {ev(0, 0x90, 60, 90), ev(5, 0x90, 64, 90)};
        k.advanceMidi(a, 10);
        CHECK(k.notePitch() == 64.0, "last held wins");
        const MidiEvent b[1] = {ev(0, 0x80, 64, 0)};
        k.advanceMidi(b, 10);
        CHECK(k.notePitch() == 60.0 && k.state() == KeyState::NoteActive, "falls back to 60");
        const MidiEvent c[1] = {ev(0, 0x90, 60, 0)};  // note-on velocity 0 == note-off
        k.advanceMidi(c, 10);
        CHECK(k.state() == KeyState::NoteHold, "vel-0 note on releases");
    }
    {  // sustain retains the note until pedal release, then hold timing starts
        KeyTracker k;
        k.prepare(fs);
        const MidiEvent a[3] = {ev(0, 0xB0, 64, 127), ev(1, 0x90, 62, 100), ev(2, 0x80, 62, 0)};
        k.advanceMidi(a, 10);
        CHECK(k.pedalDown() && k.state() == KeyState::NoteActive && k.notePitch() == 62.0, "sustained");
        advanceSecs(k, 2.0);
        CHECK(k.state() == KeyState::NoteActive && k.weight() == 1.0, "sustain keeps note for 2 s");
        const MidiEvent b[1] = {ev(0, 0xB0, 64, 0)};
        k.advanceMidi(b, 10);
        CHECK(k.state() == KeyState::NoteHold, "pedal release -> hold");
        advanceSecs(k, 0.51);
        CHECK(k.state() == KeyState::Returning, "then return");
    }
    {  // all-notes-off releases immediately (no hold), even with pedal down
        KeyTracker k;
        k.prepare(fs);
        const MidiEvent a[3] = {ev(0, 0xB0, 64, 127), ev(1, 0x90, 67, 100), ev(2, 0x80, 67, 0)};
        k.advanceMidi(a, 10);
        const MidiEvent b[1] = {ev(0, 0xB0, 123, 0)};
        k.advanceMidi(b, 4800);  // 100 ms
        CHECK(k.state() == KeyState::ManualReference && k.weight() == 0.0 && !k.pedalDown() && k.heldCount() == 0,
              "all-notes-off: immediate release, state %d w %g", int(k.state()), k.weight());
    }
    {  // block partition independence of timers
        KeyTracker a, b;
        a.prepare(fs);
        b.prepare(fs);
        const MidiEvent on[2] = {ev(3, 0x90, 60, 100), ev(7, 0x80, 60, 0)};
        a.advanceMidi(on, 4096);
        for (int i = 0; i < 4096; ++i) {
            std::vector<MidiEvent> e;
            for (const auto& x : on) if (int(x.frame) == i) { MidiEvent y = x; y.frame = 0; e.push_back(y); }
            b.advanceMidi(e, 1);
        }
        advanceSecs(a, 0.55);
        for (int i = 0; i < int(0.55 * fs); ++i) b.advanceMidi({}, 1);
        CHECK(std::fabs(a.weight() - b.weight()) < 1e-9 && a.state() == b.state(), "timer partition invariance %g %g", a.weight(), b.weight());
    }
}

static double runProcessor(ControlProcessor& cp, const core::ParamValues& p, int blocks, int frames,
                           core::ModulationBlock& mb, const std::vector<MidiEvent>* midiFirst = nullptr) {
    ControlContext ctx;
    ctx.frames = frames;
    for (int b = 0; b < blocks; ++b) {
        if (b == 0 && midiFirst) ctx.midi = std::span<const MidiEvent>(*midiFirst);
        else ctx.midi = {};
        cp.processBlock(p, ctx, AudioView{}, nullptr, mb);
    }
    return 0;
}

static void processorLock() {
    constexpr double fs = 48000.0;
    ControlProcessor cp;
    cp.prepare(fs, 480, 99);
    core::ParamValues p = core::defaultValues();
    const auto fIdx = core::indexOf("slot.a.filter.lpfFrequency");
    p[fIdx] = 1000.0;
    p[core::indexOf("key.enabled")] = 1;
    p[core::indexOf("key.scale")] = 1;   // major
    p[core::indexOf("key.root")] = 0;    // C
    p[core::indexOf("macro.drift")] = 1.0;
    p[core::indexOf("pitchDrift.enabled")] = 1;
    core::ModulationBlock mb;
    runProcessor(cp, p, 400, 480, mb);  // 4 s
    CHECK(cp.status().scaleLockActive && cp.status().pitchDriftHeld, "Resonance lock; pitch drift held");
    {   // Drift moves the target; whenever no glide is in flight the locked frequency is an in-scale note.
        int settled = 0;
        for (int b = 0; b < 600; ++b) {
            runProcessor(cp, p, 1, 480, mb);
            const double midi = scale::hzToMidi(1000.0 * std::exp2(mb.slot[0].creativeCutoffOct));
            if (std::fabs(midi - std::round(midi)) < 1e-6) {
                ++settled;
                CHECK(scale::isAllowed(scale::maskFor(1, 0), 0, int(std::lround(midi))), "settled note in scale: %g", midi);
            }
        }
        CHECK(settled > 300, "mostly settled between glides: %d", settled);
    }
    CHECK(std::fabs(mb.slot[0].creativeDelayMs) < 1e-12 && mb.slot[0].creativeDelayActive, "delay movement held at zero");
    // Without lock the creative delay moves again.
    p[core::indexOf("key.enabled")] = 0;
    runProcessor(cp, p, 400, 480, mb);
    CHECK(!cp.status().pitchDriftHeld && mb.slot[0].creativeDelayMs != 0.0, "delay released when lock off");

    // MIDI reference: held note E4 (64) moves the resonance reference there (drift 0).
    ControlProcessor c2;
    c2.prepare(fs, 480, 5);
    core::ParamValues q = core::defaultValues();
    q[fIdx] = 1000.0;
    q[core::indexOf("key.enabled")] = 1;
    q[core::indexOf("key.source")] = 1;
    q[core::indexOf("key.scale")] = 0;
    std::vector<MidiEvent> on{ev(0, 0x90, 64, 100)};
    runProcessor(c2, q, 200, 480, mb, &on);
    const double fNote = scale::midiToHz(64.0);
    CHECK(std::fabs(th::cents(1000.0 * std::exp2(mb.slot[0].creativeCutoffOct), fNote)) < 0.01, "MIDI note reference");
    std::vector<MidiEvent> off{ev(0, 0x80, 64, 0)};
    runProcessor(c2, q, 1, 480, mb, &off);
    runProcessor(c2, q, 150, 480, mb);  // 1.5 s: hold 0.5 + return 0.1 + settle
    CHECK(c2.status().keyState == KeyState::ManualReference, "returned to manual");
    CHECK(std::fabs(th::cents(1000.0 * std::exp2(mb.slot[0].creativeCutoffOct), scale::midiToHz(std::round(scale::hzToMidi(1000.0))))) < 0.01,
          "manual reference quantized to chromatic note");
}

int main() {
    scalesSettle();
    tiesAndHysteresis();
    midiTimings();
    processorLock();
    if (g_fail) { std::printf("key: %d failure(s)\n", g_fail); return 1; }
    std::printf("key: all passed\n");
    return 0;
}
