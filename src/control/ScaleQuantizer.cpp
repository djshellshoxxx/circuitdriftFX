#include "control/ScaleQuantizer.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace cd::control {
namespace {
double boundedPitch(double pitch) noexcept {
    // Keep every floor/cast and the complete local search inside the int domain.
    return std::clamp(std::isfinite(pitch) ? pitch : 69.0,
                      double(std::numeric_limits<int>::min()) + 16.0,
                      double(std::numeric_limits<int>::max()) - 16.0);
}
}  // namespace
namespace scale {
namespace {
constexpr uint16_t bits(std::initializer_list<int> iv) {
    uint16_t m = 0;
    for (int i : iv) m = uint16_t(m | (1u << i));
    return m;
}
int mod12(int v) noexcept { return ((v % 12) + 12) % 12; }
}  // namespace

uint16_t maskFor(int idx, uint16_t custom) noexcept {
    switch (idx) {
        case 1: return bits({0, 2, 4, 5, 7, 9, 11});
        case 2: return bits({0, 2, 3, 5, 7, 8, 10});
        case 3: return bits({0, 2, 4, 7, 9});
        case 4: return bits({0, 3, 5, 7, 10});
        case 5: return (custom & kChromatic) ? uint16_t(custom & kChromatic) : kChromatic;
        default: return kChromatic;
    }
}

bool isAllowed(uint16_t mask, int root, int midiNote) noexcept {
    if ((mask & kChromatic) == 0) mask = kChromatic;
    return (mask >> mod12(mod12(midiNote) - mod12(root))) & 1u;
}

int nearestAllowed(double p, uint16_t mask, int root) noexcept {
    p = boundedPitch(p);
    const int base = int(std::floor(p));
    int best = base;
    double bestD = 1e300;
    bool have = false;
    for (int n = base - 13; n <= base + 14; ++n) {  // ascending: strict '<' keeps the lower note on ties
        if (!isAllowed(mask, root, n)) continue;
        const double d = std::fabs(p - double(n));
        if (!have || d < bestD) { best = n; bestD = d; have = true; }
    }
    return best;
}
}  // namespace scale

void ResonanceQuantizer::configure(uint16_t mask, int root) noexcept {
    const uint16_t m = (mask & scale::kChromatic) ? uint16_t(mask & scale::kChromatic) : scale::kChromatic;
    const int r = ((root % 12) + 12) % 12;
    if (m != mask_ || r != root_) {
        mask_ = m;
        root_ = r;
        if (haveNote_ && !scale::isAllowed(mask_, root_, note_)) haveNote_ = false;  // discard decision, retain glide
    }
}

int ResonanceQuantizer::decide(double p) noexcept {
    p = boundedPitch(p);
    const int cand = scale::nearestAllowed(p, mask_, root_);
    if (!haveNote_) { note_ = cand; haveNote_ = true; return note_; }
    if (cand != note_) {
        const double dCur = std::fabs(p - double(note_)), dCand = std::fabs(p - double(cand));
        if (dCur - dCand > 2.0 * kHysteresisSt) note_ = cand;  // crossed the midpoint by >0.15 st
    }
    return note_;
}

double ResonanceQuantizer::update(double p, double dt) noexcept {
    const int n = decide(p);
    if (!haveOutput_) {
        current_ = start_ = double(n);
        glideRemaining_ = 0;
        haveOutput_ = true;
    } else if (n != glideNote_) {
        start_ = current_;
        glideRemaining_ = kGlideS;
    }
    glideNote_ = n;
    if (glideRemaining_ > 0 && dt > 0) {
        glideRemaining_ -= dt;
        if (glideRemaining_ <= 0) { glideRemaining_ = 0; current_ = double(n); }
        else current_ = start_ + (double(n) - start_) * (1.0 - glideRemaining_ / kGlideS);
    } else if (glideRemaining_ <= 0) {
        current_ = double(n);
    }
    return current_;
}

}  // namespace cd::control
