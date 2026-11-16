// T12 (CD-F12) scale quantizer + MIDI note tracker checks. Synthetic frequencies only.
#include "control/ScaleQuantizer.h"
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
constexpr double kFs = 48000.0;

double centsBetween(double a, double b) { return 1200.0 * std::log2(a / b); }
double hz(int note, double a4 = 440.0) { return a4 * std::exp2((note - 69) / 12.0); }

void masks() {
    CHECK(scaleMask(Scale::Major, 0) == 0b101010110101);
    CHECK(scaleMask(Scale::NaturalMinor, 0) == 0b010110101101);
    CHECK(scaleMask(Scale::PentatonicMajor, 0) == 0b001010010101);
    CHECK(scaleMask(Scale::PentatonicMinor, 0) == 0b010010101001);
    CHECK(scaleMask(Scale::Chromatic, 0) == 0xFFF);
    CHECK(scaleMask(Scale::Custom, 0) == 0);
    CHECK(scaleMask(Scale::Custom, 0x1000) == 0);
    CHECK(scaleMask(Scale::Custom, 0x081) == 0x081);
    ScaleQuantizer q;
    CHECK(!q.configure(Scale::Custom, 0, 0, 440.0, kFs));
    CHECK(!q.configure(Scale::Major, 0, 12, 440.0, kFs));
    CHECK(!q.configure(Scale::Major, 0, -1, 440.0, kFs));
    CHECK(!q.configure(Scale::Major, 0, 0, kNan, kFs));
    CHECK(!q.configure(Scale::Major, 0, 0, 440.0, 0.0));
    CHECK(!q.configured());
    CHECK(q.configure(Scale::Major, 0, 0, 440.0, kFs));
    CHECK(!q.configure(Scale::Custom, 0, 0, 440.0, kFs) && q.configured());  // old config kept
}

void settlesWithinCents() {
    const Scale scales[] = {Scale::Chromatic, Scale::Major, Scale::NaturalMinor, Scale::PentatonicMajor,
                            Scale::PentatonicMinor, Scale::Custom};
    double worst = 0.0;
    int cases = 0;
    for (const Scale s : scales) {
        for (int root = 0; root < 12; ++root) {
            ScaleQuantizer q;
            CHECK(q.configure(s, 0b100100010001, root, 440.0, kFs));
            const std::uint16_t mask = scaleMask(s, 0b100100010001);
            for (double f = 55.0; f < 4000.0; f *= 1.0123) {
                CHECK(q.setTarget(f));
                int expect = 0;
                CHECK(q.nearestNote(q.pitchOf(f), expect));
                const int pc = ((expect - root) % 12 + 12) % 12;
                CHECK(((mask >> pc) & 1) != 0);
                q.advance(48000);  // 1 s >> 50 ms glide
                const double err = std::fabs(centsBetween(q.outputHz(), hz(q.note())));
                worst = std::max(worst, err);
                CHECK(err < 0.01);
                ++cases;
            }
        }
    }
    std::printf("T12 quantizer: %d cases, worst settled error %.3e cents (limit 0.01)\n", cases, worst);
}

void tiesAndHysteresis() {
    ScaleQuantizer q;
    q.configure(Scale::Major, 0, 0, 440.0, kFs);  // C major: 61 not allowed
    int n = 0;
    CHECK(q.nearestNote(60.5, n) && n == 60);
    CHECK(q.nearestNote(61.0, n) && n == 60);  // exactly between 60 and 62: lower wins
    ScaleQuantizer c;
    c.configure(Scale::Chromatic, 0, 0, 440.0, kFs);
    CHECK(c.nearestNote(60.5, n) && n == 60);  // tie goes lower
    CHECK(c.nearestNote(-0.5, n) && n == -1);  // tie between -1 and 0 goes lower
    // Hysteresis: chromatic, hold 60; boundary at 60.5, margin 0.15.
    CHECK(c.setTarget(hz(60)));
    CHECK(c.note() == 60);
    c.setTarget(440.0 * std::exp2((60.64 - 69) / 12.0));
    CHECK(c.note() == 60);  // inside 0.15 margin
    c.setTarget(440.0 * std::exp2((60.66 - 69) / 12.0));
    CHECK(c.note() == 61);  // beyond margin
    c.setTarget(440.0 * std::exp2((60.36 - 69) / 12.0));
    CHECK(c.note() == 61);  // 61 holds until below 60.35
    c.setTarget(440.0 * std::exp2((60.34 - 69) / 12.0));
    CHECK(c.note() == 60);
    // Determinism: identical sequences give identical states.
    ScaleQuantizer a, b;
    a.configure(Scale::PentatonicMinor, 0, 9, 442.0, kFs);
    b.configure(Scale::PentatonicMinor, 0, 9, 442.0, kFs);
    for (double f = 100; f < 900; f += 3.7) {
        a.setTarget(f); b.setTarget(f);
        a.advance(100); b.advance(100);
        CHECK(a.note() == b.note() && a.outputPitch() == b.outputPitch());
    }
}

void invalidInput() {
    ScaleQuantizer q;
    CHECK(!q.setTarget(440.0));  // unconfigured
    q.configure(Scale::Major, 0, 0, 440.0, kFs);
    CHECK(q.setTarget(440.0));
    const int note = q.note();
    const double p = q.outputPitch();
    for (const double bad : {kNan, kInf, -kInf, 0.0, -100.0, -0.0}) {
        CHECK(!q.setTarget(bad));
        CHECK(q.note() == note && q.outputPitch() == p);
    }
    CHECK(!q.setTarget(1e-300) || std::isfinite(q.outputPitch()));
    CHECK(!q.setTarget(1e300) || std::isfinite(q.outputPitch()));
    int n;
    CHECK(!q.nearestNote(kNan, n) && !q.nearestNote(kInf, n));
    CHECK(std::isnan(q.pitchOf(-1.0)) && std::isnan(q.pitchOf(kNan)));
    q.advance(0);
}

void glide() {
    ScaleQuantizer q;
    q.configure(Scale::Chromatic, 0, 0, 440.0, kFs);
    q.setTarget(hz(60));
    q.setTarget(hz(72));  // far jump, 12 semitones
    q.advance(2400);       // 50 ms: ~95% of the way
    const double frac = (q.outputPitch() - 60.0) / 12.0;
    CHECK(std::fabs(frac - 0.95) < 0.005);
    CHECK(q.outputPitch() < 72.0);
}

void midi() {
    MidiNoteTracker m;
    CHECK(!m.hasActiveNote() && m.weight() == 0.0);
    m.noteOn(60);
    m.noteOn(64);
    CHECK(m.activeNote() == 64);  // last held wins
    m.noteOff(64);
    CHECK(m.activeNote() == 60);  // falls back to remaining held
    m.noteOn(67);
    m.sustain(true);
    m.noteOff(67);
    CHECK(m.activeNote() == 67);  // sustained
    m.noteOn(72);
    m.noteOff(72);
    CHECK(m.activeNote() == 72);  // last released under pedal still last
    m.sustain(false);
    CHECK(m.activeNote() == 60);  // 60 still physically held
    m.noteOff(60);
    CHECK(!m.hasActiveNote() && m.referenceNote() == 60);
    // Hold 500 ms then 100 ms return.
    CHECK(m.weight() == 1.0);
    m.advance(0.499);
    CHECK(m.weight() == 1.0);
    m.advance(0.051);  // 50 ms into ramp
    CHECK(std::fabs(m.weight() - 0.5) < 1e-9);
    m.advance(0.05);
    CHECK(m.weight() == 0.0);
    // All notes off: immediate release of held and sustained; hold timer starts.
    m.noteOn(50);
    m.noteOn(55);
    m.sustain(true);
    m.noteOff(55);
    m.allNotesOff();
    CHECK(!m.hasActiveNote());
    m.sustain(false);
    CHECK(!m.hasActiveNote());
    CHECK(m.weight() == 1.0 && m.referenceNote() == 55);
    m.advance(0.7);
    CHECK(m.weight() == 0.0);
    // Invalid input ignored.
    m.noteOn(-1); m.noteOn(128); m.noteOff(200); m.advance(kNan); m.advance(-1.0);
    CHECK(!m.hasActiveNote());
    // Re-press during ramp restores full weight.
    m.noteOn(40);
    CHECK(m.weight() == 1.0 && m.activeNote() == 40);
}
}  // namespace

int main() {
    masks();
    settlesWithinCents();
    tiesAndHysteresis();
    invalidInput();
    glide();
    midi();
    if (failures == 0) std::printf("control key: all T12 quantizer/MIDI checks passed\n");
    return failures == 0 ? 0 : 1;
}
