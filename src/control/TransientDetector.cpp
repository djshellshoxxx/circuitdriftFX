#include "control/TransientDetector.h"
#include <algorithm>
#include <cmath>

namespace cd::control {
namespace {
double coeff(double fs, double tauS) noexcept { return std::exp(-1.0 / (fs * tauS)); }
}

void TransientDetector::prepare(double fs) noexcept {
    if (!(fs > 0)) fs = 48000.0;
    aFastA_ = coeff(fs, 0.0002);
    aFastR_ = coeff(fs, 0.010);
    aSlowA_ = coeff(fs, 0.020);
    aSlowR_ = coeff(fs, 0.100);
    aHoldRel_ = coeff(fs, 0.020);
    holdSamples_ = std::max(1, int(std::lround(0.005 * fs)));
    reset();
}

void TransientDetector::reset() noexcept {
    fast_ = slow_ = held_ = 0;
    holdCount_ = 0;
}

void TransientDetector::process(const double* L, const double* R, int frames, double amount, double* out) noexcept {
    const double amt = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.0;
    constexpr double kGate = 1e-3;  // -60 dBFS
    for (int n = 0; n < frames; ++n) {
        double p = L ? std::fabs(L[n]) : 0.0;
        if (R) p = std::max(p, std::fabs(R[n]));
        if (!std::isfinite(p)) p = 0.0;
        fast_ = p > fast_ ? p + aFastA_ * (fast_ - p) : p + aFastR_ * (fast_ - p);
        slow_ = p > slow_ ? p + aSlowA_ * (slow_ - p) : p + aSlowR_ * (slow_ - p);
        double b = 0.0;
        if (fast_ >= kGate) {
            const double db = 20.0 * std::log10((fast_ + 1e-9) / (slow_ + 1e-9));
            b = std::clamp((db - 3.0) / 9.0, 0.0, 1.0);
        }
        if (b >= held_) {
            held_ = b;
            holdCount_ = holdSamples_;
        } else if (holdCount_ > 0) {
            --holdCount_;
        } else {
            held_ *= aHoldRel_;
        }
        out[n] = amt * held_;
    }
}

}  // namespace cd::control
