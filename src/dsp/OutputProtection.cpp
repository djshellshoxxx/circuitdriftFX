#include "OutputProtection.h"

namespace cd::dsp {

bool ButterworthHpf::design(double fs, double fc) noexcept {
    if (!(fs > 0.0) || !(fc > 0.0) || !(fc < 0.5 * fs)) return false;
    const double k = std::tan(M_PI * fc / fs);
    const double r2 = std::sqrt(2.0);
    const double norm = 1.0 / (1.0 + r2 * k + k * k);
    b0 = norm; b1 = -2.0 * norm; b2 = norm;
    a1 = 2.0 * (k * k - 1.0) * norm;
    a2 = (1.0 - r2 * k + k * k) * norm;
    return true;
}

void GainMatcher::prepare(double fs) noexcept {
    aEnv_ = std::exp(-1.0 / (fs * 0.4));
    aGain_ = std::exp(-1.0 / (fs * 1.0));
    reset();
}
void GainMatcher::reset() noexcept { ed_ = ew_ = logGain_ = 0.0; }

double GainMatcher::update(double dry, double wet) noexcept {
    ed_ = aEnv_ * ed_ + (1.0 - aEnv_) * dry * dry;
    ew_ = aEnv_ * ew_ + (1.0 - aEnv_) * wet * wet;
    constexpr double kFloor = 1e-6;  // (-60 dBFS)^2 mean-square
    if (ed_ > kFloor && ew_ > kFloor) {
        const double target = std::clamp(std::sqrt((ed_ + 1e-12) / (ew_ + 1e-12)), 0.25, 4.0);
        logGain_ = aGain_ * logGain_ + (1.0 - aGain_) * std::log(target);
    }
    return std::exp(logGain_);
}

}  // namespace cd::dsp
