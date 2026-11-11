#include "control/Dynamics.h"
#include "core/ParameterRegistry.h"
#include <algorithm>
#include <cmath>

namespace cd::control {
namespace {
using core::smoothingCoeff;
double sane(double x, bool& flag) noexcept {
    if (std::isfinite(x)) return x;
    flag = true;
    return 0.0;
}
}

bool TransientDetector::prepare(double fs) noexcept {
    if (!std::isfinite(fs) || !(fs > 0)) return false;
    aFA_ = smoothingCoeff(fs, 0.0002); aFR_ = smoothingCoeff(fs, 0.010);
    aSA_ = smoothingCoeff(fs, 0.020);  aSR_ = smoothingCoeff(fs, 0.100);
    aHR_ = smoothingCoeff(fs, 0.020);
    holdSamples_ = static_cast<int>(std::lround(0.005 * fs));
    reset();
    return true;
}
void TransientDetector::reset() noexcept { fast_ = slow_ = held_ = 0; holdLeft_ = 0; sanitized_ = false; }

double TransientDetector::process(double l, double r) noexcept {
    const double p = std::max(std::fabs(sane(l, sanitized_)), std::fabs(sane(r, sanitized_)));
    fast_ = p + ((p > fast_) ? aFA_ : aFR_) * (fast_ - p);
    slow_ = p + ((p > slow_) ? aSA_ : aSR_) * (slow_ - p);
    double b = (20.0 * std::log10((fast_ + 1e-9) / (slow_ + 1e-9)) - 3.0) / 9.0;
    b = std::clamp(b, 0.0, 1.0);
    if (fast_ < 1e-3) b = 0.0;  // -60 dBFS gate
    if (b >= held_) { held_ = b; holdLeft_ = holdSamples_; }
    else if (holdLeft_ > 0) --holdLeft_;
    else held_ = b + aHR_ * (held_ - b);
    return held_;
}
double TransientDetector::strength(double amount, double b) noexcept {
    return std::isfinite(amount) && std::isfinite(b) ? std::clamp(amount, 0.0, 1.0) * std::clamp(b, 0.0, 1.0) : 0.0;
}
double TransientDetector::effectiveR(double r, double P) noexcept { return r + P * (1.0 - r); }

bool SidechainLoadDetector::prepare(double fs, const Config& c) noexcept {
    if (!std::isfinite(fs) || !(fs > 0)) return false;
    Config n = c;
    n.hpfHz = std::isfinite(c.hpfHz) ? std::clamp(c.hpfHz, 20.0, 300.0) : 40.0;
    n.depth = std::isfinite(c.depth) ? std::clamp(c.depth, 0.0, 2.0) : 0.0;
    n.iBudgetAmps = std::isfinite(c.iBudgetAmps) ? std::max(0.0, c.iBudgetAmps) : 0.0;
    dsp::ButterworthHpf h;
    if (!h.design(fs, n.hpfHz)) return false;
    cfg_ = n; hpfL_ = hpfR_ = h;
    aMs_ = smoothingCoeff(fs, 0.010); aA_ = smoothingCoeff(fs, 0.001); aR_ = smoothingCoeff(fs, 0.050);
    reset();
    return true;
}
void SidechainLoadDetector::reset() noexcept { hpfL_.reset(); hpfR_.reset(); ms_ = env_ = 0; sanitized_ = false; }

double SidechainLoadDetector::process(double l, double r) noexcept {
    if (!connected_ || cfg_.source == Source::Main) { ms_ = env_ = 0; return 0.0; }  // idle: no stale energy
    const double hl = hpfL_.process(sane(l, sanitized_)), hr = hpfR_.process(sane(r, sanitized_));
    const double p = std::max(std::fabs(hl), std::fabs(hr));  // linked max channel magnitude
    ms_ = p * p + aMs_ * (ms_ - p * p);
    env_ = ms_ + ((ms_ > env_) ? aA_ : aR_) * (env_ - ms_);
    constexpr double kERef = 0.12589254117941673;  // -18 dBFS RMS
    const double ratio = std::clamp(std::sqrt(std::max(env_, 0.0)) / kERef, 0.0, 4.0);
    return cfg_.depth * cfg_.iBudgetAmps * ratio;
}

}  // namespace cd::control
