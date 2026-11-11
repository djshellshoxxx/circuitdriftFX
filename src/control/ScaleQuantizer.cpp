#include "control/ScaleQuantizer.h"
#include <cmath>
#include <initializer_list>

namespace cd::control {

std::uint16_t scaleMask(Scale s, std::uint16_t custom) noexcept {
    auto m = [](std::initializer_list<int> v) {
        std::uint16_t r = 0;
        for (int i : v) r = static_cast<std::uint16_t>(r | (1u << i));
        return r;
    };
    switch (s) {
        case Scale::Chromatic: return 0xFFF;
        case Scale::Major: return m({0, 2, 4, 5, 7, 9, 11});
        case Scale::NaturalMinor: return m({0, 2, 3, 5, 7, 8, 10});
        case Scale::PentatonicMajor: return m({0, 2, 4, 7, 9});
        case Scale::PentatonicMinor: return m({0, 3, 5, 7, 10});
        case Scale::Custom: return (custom != 0 && custom <= 0xFFF) ? custom : 0;
    }
    return 0;
}

bool ScaleQuantizer::configure(Scale s, std::uint16_t custom, int root, double a4, double fs) noexcept {
    const std::uint16_t mask = scaleMask(s, custom);
    if (mask == 0 || root < 0 || root > 11) return false;
    if (!std::isfinite(a4) || a4 < 400.0 || a4 > 480.0) return false;
    if (!std::isfinite(fs) || fs < 8000.0 || fs > 768000.0) return false;
    mask_ = mask;
    root_ = root;
    a4_ = a4;
    fs_ = fs;
    glideCoef_ = -std::expm1(-std::log(20.0) / (kGlideSeconds * fs));
    configured_ = true;
    haveNote_ = false;
    return true;
}

bool ScaleQuantizer::allowed(int note) const noexcept {
    const int pc = ((note - root_) % 12 + 12) % 12;
    return ((mask_ >> pc) & 1u) != 0;
}

bool ScaleQuantizer::nearestNote(double p, int& note) const noexcept {
    if (!configured_ || !std::isfinite(p) || std::fabs(p) > 1e4) return false;
    const int c = static_cast<int>(std::floor(p + 0.5));
    int best = 0;
    double bd = 1e300;
    bool found = false;
    for (int n = c - 12; n <= c + 12; ++n) {  // ascending: a tie keeps the lower note
        if (!allowed(n)) continue;
        const double d = std::fabs(p - n);
        if (!found || d < bd - 1e-12) {
            best = n;
            bd = d;
            found = true;
        }
    }
    if (!found) return false;
    note = best;
    return true;
}

double ScaleQuantizer::pitchOf(double hz) const noexcept {
    if (!std::isfinite(hz) || hz <= 0.0) return std::nan("");
    return 69.0 + 12.0 * std::log2(hz / a4_);
}
double ScaleQuantizer::hzOf(double p) const noexcept { return a4_ * std::exp2((p - 69.0) / 12.0); }

bool ScaleQuantizer::setTarget(double hz) noexcept {
    if (!configured_) return false;
    const double p = pitchOf(hz);
    int cand = 0;
    if (!nearestNote(p, cand)) return false;
    if (!haveNote_ || !allowed(note_)) {
        note_ = cand;
        glidePitch_ = cand;
        haveNote_ = true;  // first target snaps
        return true;
    }
    if (cand != note_) {
        // Switch only once p has passed the boundary by the hysteresis margin.
        if (std::fabs(p - note_) - std::fabs(p - cand) > 2.0 * kHysteresisSemitones) note_ = cand;
    }
    return true;
}

void ScaleQuantizer::step() noexcept {
    if (!haveNote_) return;
    glidePitch_ += glideCoef_ * (static_cast<double>(note_) - glidePitch_);
}
void ScaleQuantizer::advance(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) step();
}

void MidiNoteTracker::reset() noexcept { *this = MidiNoteTracker{}; }

void MidiNoteTracker::recompute() noexcept {
    int best = -1;
    std::uint64_t bo = 0;
    for (int n = 0; n < 128; ++n)
        if ((held_[n] || sustained_[n]) && order_[n] >= bo) {
            best = n;
            bo = order_[n];
        }
    if (best >= 0) {
        active_ = best;
        lastNote_ = best;
        idle_ = 0.0;
    } else if (active_ >= 0) {
        active_ = -1;
        idle_ = 0.0;
    }
}

void MidiNoteTracker::noteOn(int note) noexcept {
    if (note < 0 || note > 127) return;
    held_[note] = true;
    sustained_[note] = false;
    order_[note] = ++counter_;
    recompute();
}
void MidiNoteTracker::noteOff(int note) noexcept {
    if (note < 0 || note > 127 || !held_[note]) return;
    held_[note] = false;
    if (pedal_) sustained_[note] = true;
    recompute();
}
void MidiNoteTracker::sustain(bool down) noexcept {
    pedal_ = down;
    if (!down)
        for (int n = 0; n < 128; ++n) sustained_[n] = false;
    recompute();
}
void MidiNoteTracker::allNotesOff() noexcept {
    for (int n = 0; n < 128; ++n) held_[n] = sustained_[n] = false;
    recompute();
}
void MidiNoteTracker::advance(double s) noexcept {
    if (!std::isfinite(s) || s < 0.0) return;
    if (active_ < 0 && idle_ < 1e8) idle_ += s;
}
double MidiNoteTracker::weight() const noexcept {
    if (lastNote_ < 0) return 0.0;
    if (active_ >= 0 || idle_ <= kHoldSeconds) return 1.0;
    const double w = 1.0 - (idle_ - kHoldSeconds) / kReturnSeconds;
    return w < 0.0 ? 0.0 : w;
}

}  // namespace cd::control
