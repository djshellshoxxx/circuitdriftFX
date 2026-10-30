#include "OutputProtection.h"

namespace cd::dsp {

bool ButterworthHpf::design(double fs, double fc) noexcept {
    if (!(fs > 0.0) || !(fc > 0.0) || !std::isfinite(fs) || !std::isfinite(fc) || !(fc < 0.5 * fs)) return false;
    const double k = std::tan(M_PI * (fc / fs));
    const double r2 = std::sqrt(2.0);
    const double norm = 1.0 / (1.0 + r2 * k + k * k);
    const double nextA1 = 2.0 * (k * k - 1.0) * norm;
    const double nextA2 = (1.0 - r2 * k + k * k) * norm;
    if (!(k > 0.0) || !(norm > 0.0) || !std::isfinite(norm) ||
        !std::isfinite(nextA1) || !std::isfinite(nextA2)) return false;
    b0 = norm; b1 = -2.0 * norm; b2 = norm;
    a1 = nextA1;
    a2 = nextA2;
    return true;
}

void GainMatcher::prepare(double fs) noexcept {
    if (!(fs > 0.0) || !std::isfinite(fs)) return;
    aEnv_ = std::exp(-1.0 / (fs * 0.4));
    aGain_ = std::exp(-1.0 / (fs * 1.0));
    reset();
}
void GainMatcher::reset() noexcept { ed_ = ew_ = logGain_ = 0.0; }

double GainMatcher::update(double dry, double wet) noexcept {
    return updateMeanSquares(dry * dry, wet * wet);
}

double GainMatcher::updateMeanSquares(double dryPower, double wetPower) noexcept {
    if (!(dryPower >= 0.0) || !(wetPower >= 0.0) || !std::isfinite(dryPower) || !std::isfinite(wetPower)) return gain();
    const double ed = aEnv_ * ed_ + (1.0 - aEnv_) * dryPower;
    const double ew = aEnv_ * ew_ + (1.0 - aEnv_) * wetPower;
    if (!std::isfinite(ed) || !std::isfinite(ew)) return gain();
    double nextLogGain = logGain_;
    constexpr double kFloor = 1e-6;  // (-60 dBFS)^2 mean-square
    if (ed > kFloor && ew > kFloor) {
        // sqrt(a)/sqrt(b) avoids overflowing an otherwise valid power ratio before clamping.
        const double target = std::clamp(std::sqrt(ed + 1e-12) / std::sqrt(ew + 1e-12), 0.25, 4.0);
        nextLogGain = aGain_ * logGain_ + (1.0 - aGain_) * std::log(target);
        if (!std::isfinite(nextLogGain)) return gain();
    }
    ed_ = ed;
    ew_ = ew;
    logGain_ = nextLogGain;
    return std::exp(logGain_);
}

}  // namespace cd::dsp
