#include "control/SidechainLoad.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace cd::control {

void SidechainLoad::prepare(double fs) noexcept {
    fs_ = fs > 0 ? fs : 48000.0;
    aMs_ = std::exp(-1.0 / (fs_ * 0.010));
    aAtk_ = std::exp(-1.0 / (fs_ * 0.001));
    aRel_ = std::exp(-1.0 / (fs_ * 0.050));
    hpfHz_ = -1;
    reset();
}

void SidechainLoad::reset() noexcept {
    hp_[0] = hp_[1] = Biquad{};
    ms_ = env_ = lastRatio_ = 0;
}

void SidechainLoad::design(double hz) noexcept {
    hz = std::clamp(std::isfinite(hz) ? hz : 40.0, 20.0, 300.0);
    if (hz == hpfHz_) return;
    hpfHz_ = hz;
    const double w0 = 2.0 * std::numbers::pi * hz / fs_;
    const double cs = std::cos(w0), al = std::sin(w0) / (2.0 * 0.70710678118654752);
    const double a0 = 1.0 + al;
    // RBJ high-pass, transposed direct form II coefficients normalized by a0.
    c_[0] = ((1.0 + cs) * 0.5) / a0;
    c_[1] = -(1.0 + cs) / a0;
    c_[2] = ((1.0 + cs) * 0.5) / a0;
    c_[3] = (-2.0 * cs) / a0;
    c_[4] = (1.0 - al) / a0;
}

bool SidechainLoad::process(SidechainSource source, double depth, double hpfHz, const double* extL,
                            const double* extR, bool connected, int frames, double* out) noexcept {
    const bool use = connected && extL && source != SidechainSource::Main;
    const double d = std::isfinite(depth) ? std::clamp(depth, 0.0, 2.0) : 0.0;
    if (!use) {
        for (int n = 0; n < frames; ++n) out[n] = 0.0;
        reset();  // idle detector holds no stale energy; reconnect starts clean
        return false;
    }
    design(hpfHz);
    const double scale = d * budget_ / kRefRms;
    for (int n = 0; n < frames; ++n) {
        double m = std::fabs(hp_[0].run(extL[n], c_));
        if (extR) m = std::max(m, std::fabs(hp_[1].run(extR[n], c_)));
        if (!std::isfinite(m)) m = 0.0;
        ms_ = m * m + aMs_ * (ms_ - m * m);
        const double rms = std::sqrt(ms_);
        env_ = rms > env_ ? rms + aAtk_ * (env_ - rms) : rms + aRel_ * (env_ - rms);
        lastRatio_ = env_ / kRefRms;
        out[n] = scale * kRefRms * std::clamp(lastRatio_, 0.0, 4.0);
    }
    return true;
}

}  // namespace cd::control
