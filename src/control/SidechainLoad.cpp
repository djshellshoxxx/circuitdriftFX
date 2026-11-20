#include "control/SidechainLoad.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace cd::control {
namespace {
// Squaring this ceiling reaches 2^1000, leaving 24 binary exponents below binary64 overflow.
// With |b0,b2,a2| <= 1 and |b1,a1| <= 2, bounded inputs/states make every candidate below 7x
// this ceiling before validation. The accepted output can then be squared without overflow.
constexpr double kMaxDetectorMagnitude = 0x1p500;
constexpr double kMaxBudgetAmps = std::numeric_limits<double>::max() / 8.0;

bool withinDetectorDomain(double value) noexcept {
    return std::isfinite(value) && std::fabs(value) <= kMaxDetectorMagnitude;
}
}  // namespace

double SidechainLoad::Biquad::run(double x, const double* c) noexcept {
    // Treat malformed/out-of-domain input as silence before it can enter either delay state.
    x = withinDetectorDomain(x) ? x : 0.0;
    const double y = c[0] * x + z1;
    const double nextZ1 = c[1] * x - c[3] * y + z2;
    const double nextZ2 = c[2] * x - c[4] * y;
    if (!withinDetectorDomain(y) || !withinDetectorDomain(nextZ1) || !withinDetectorDomain(nextZ2)) {
        // Commit neither candidate: keep the failure local to this channel and permit recovery.
        z1 = z2 = 0.0;
        return 0.0;
    }
    z1 = nextZ1;
    z2 = nextZ2;
    return y;
}

void SidechainLoad::prepare(double fs) noexcept {
    fs_ = std::isfinite(fs) && fs >= 8000.0 && fs <= 768000.0 ? fs : 48000.0;
    aMs_ = std::exp(-1.0 / (fs_ * 0.010));
    aAtk_ = std::exp(-1.0 / (fs_ * 0.001));
    aRel_ = std::exp(-1.0 / (fs_ * 0.050));
    hpfHz_ = -1;
    design(40.0);  // coefficient identity always corresponds to the effective prepared rate
    reset();
}

void SidechainLoad::setBudgetAmps(double amps) noexcept {
    budget_ = std::isfinite(amps) && amps > 0.0 ? std::min(amps, kMaxBudgetAmps) : 0.0;
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
    design(hpfHz);  // parameter updates apply even when the callback contains no samples
    if (frames <= 0) return use;
    const double d = std::isfinite(depth) ? std::clamp(depth, 0.0, 2.0) : 0.0;
    if (!use) {
        for (int n = 0; n < frames; ++n) out[n] = 0.0;
        reset();  // idle detector holds no stale energy; reconnect starts clean
        return false;
    }
    // budget_ <= DBL_MAX/8 and d <= 2. Multiplication by the final ratio <= 4 is safe;
    // avoid the old divide/multiply by kRefRms, whose intermediate could overflow.
    const double scale = d > 0.0 ? d * budget_ : 0.0;
    for (int n = 0; n < frames; ++n) {
        double m = std::fabs(hp_[0].run(extL[n], c_));
        if (extR) m = std::max(m, std::fabs(hp_[1].run(extR[n], c_)));
        const double squared = m * m;
        ms_ = squared + aMs_ * (ms_ - squared);
        const double rms = std::sqrt(ms_);
        env_ = rms > env_ ? rms + aAtk_ * (env_ - rms) : rms + aRel_ * (env_ - rms);
        lastRatio_ = env_ / kRefRms;
        out[n] = scale * std::clamp(lastRatio_, 0.0, 4.0);
    }
    return true;
}

}  // namespace cd::control
