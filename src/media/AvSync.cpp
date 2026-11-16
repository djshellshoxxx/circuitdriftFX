#include "media/AvSync.h"
#include <cmath>

namespace cd::media {

void AvSyncModel::record(std::int64_t tick, std::int64_t content) noexcept {
    const double x = double(tick) / double(rate_);                 // seconds
    const double y = double(tick - content) * 1000.0 / double(rate_);  // ms
    if (n_ == 0) { minX_ = maxX_ = x; }
    if (x < minX_) minX_ = x;
    if (x > maxX_) maxX_ = x;
    ++n_;
    sx_ += x; sy_ += y; sxy_ += x * y; sxx_ += x * x;
    if (std::fabs(y) > maxAbs_) maxAbs_ = std::fabs(y);
}

double AvSyncModel::driftMs() const noexcept {
    if (n_ < 2) return 0.0;
    const double nn = double(n_);
    const double den = nn * sxx_ - sx_ * sx_;
    if (std::fabs(den) < 1e-12) return 0.0;
    const double slope = (nn * sxy_ - sx_ * sy_) / den;  // ms per second
    return slope * (maxX_ - minX_);
}

void ClockMapper::observe(double wall, std::int64_t sample) noexcept {
    if (!std::isfinite(wall)) return;
    wall_[head_] = wall; smp_[head_] = double(sample);
    head_ = (head_ + 1) % kN;
    if (count_ < kN) ++count_;
}

std::int64_t ClockMapper::sampleAt(double wall) const noexcept {
    if (count_ == 0) return std::int64_t(std::llround(wall * nominal_));
    double mw = 0, ms = 0;
    for (int i = 0; i < count_; ++i) { mw += wall_[i]; ms += smp_[i]; }
    mw /= count_; ms /= count_;
    double sxx = 0, sxy = 0;
    for (int i = 0; i < count_; ++i) {
        const double dx = wall_[i] - mw;
        sxx += dx * dx; sxy += dx * (smp_[i] - ms);
    }
    const double slope = (count_ >= 2 && sxx > 1e-12) ? sxy / sxx : nominal_;
    return std::int64_t(std::llround(ms + slope * (wall - mw)));
}

}  // namespace cd::media
