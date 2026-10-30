#include "dsp/Halfband2x.h"

#include <cmath>

namespace cd::dsp {

namespace {

// Zeroth-order modified Bessel function of the first kind (power series, converges fast for beta<~20).
double besselI0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 200; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

}  // namespace

void Halfband2x::prepare() noexcept {
    // Kaiser beta for 100 dB stopband attenuation.
    constexpr double kBeta = 10.0623;
    constexpr double kCutoff = 0.25;  // fraction of fs_hi
    const double centre = double(kHalf);
    const double i0Beta = besselI0(kBeta);
    double dcGain = 0.0;
    for (std::size_t n = 0; n < kTaps; ++n) {
        const double m = double(n) - centre;
        const double r = m / centre;
        const double window = besselI0(kBeta * std::sqrt(1.0 - r * r)) / i0Beta;
        const double arg = 2.0 * kCutoff * m;
        const double sinc = arg == 0.0 ? 1.0 : std::sin(M_PI * arg) / (M_PI * arg);
        h_[n] = 2.0 * kCutoff * sinc * window;
        dcGain += h_[n];
    }
    // Normalise DC gain of the low-pass to exactly 1.
    for (auto& c : h_) c /= dcGain;
    reset();
}

void Halfband2x::reset() noexcept {
    z_.fill(0.0);
    pos_ = 0;
}

}  // namespace cd::dsp
