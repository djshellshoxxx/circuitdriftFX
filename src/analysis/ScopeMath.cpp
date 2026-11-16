// CircuitDrift analysis: thermal/XY scope math implementation. See ScopeMath.h.
#include "analysis/ScopeMath.h"
#include <cmath>
#include <limits>

namespace cd::analysis {

ScopeModeInfo scopeModeInfo(ScopeMode mode, bool magnetic, bool railIsFraction) noexcept {
    switch (mode) {
        case ScopeMode::TransferXY:
            return {true, "Input (V)", "Output (V)", ""};
        case ScopeMode::VoltageTemperature:
            return {true, "Voltage (V)", "Temperature (\xC2\xB0""C)", ""};
        case ScopeMode::VoltageRail:
            return {true, "Input (V)", railIsFraction ? "Rail (fraction of nominal)" : "Rail (V)", ""};
        case ScopeMode::MagneticBH:
            if (magnetic) return {true, "H (A/m)", "B (T)", ""};
            return {false, "H (A/m)", "B (T)", "model does not expose magnetic state"};
    }
    return {};
}

XyMapper::XyMapper(int w, int h, double vpp) noexcept : w_(w), h_(h) {
    valid_ = w > 0 && h > 0 && std::isfinite(vpp) && vpp > 0.0;
    if (!valid_) return;
    cx_ = 0.5 * w;
    cy_ = 0.5 * h;
    inv_ = 1.0 / vpp;
}

PixelPoint XyMapper::map(double xv, double yv) const noexcept {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!valid_ || !std::isfinite(xv) || !std::isfinite(yv)) return {nan, nan};
    return {cx_ + xv * inv_, cy_ - yv * inv_};
}

PixelPoint XyMapper::mapRounded(double xv, double yv) const noexcept {
    PixelPoint p = map(xv, yv);
    // Pixel centres at integer + 0.5; snap the continuous position to the nearest centre.
    p.x = std::floor(p.x) + 0.5;
    p.y = std::floor(p.y) + 0.5;
    return p;
}

bool XyMapper::onScreen(PixelPoint p) const noexcept {
    return valid_ && p.x >= 0 && p.y >= 0 && p.x < w_ && p.y < h_;
}

std::size_t decimateExtrema(const XyPair* in, std::size_t n, XyPair* out, std::size_t maxOut) noexcept {
    if (!in || !out || n == 0) return 0;
    if (maxOut < 2) maxOut = 2;
    std::size_t w = 0;
    auto finite = [](const XyPair& p) { return std::isfinite(p.x) && std::isfinite(p.y); };
    if (n <= maxOut) {
        for (std::size_t i = 0; i < n; ++i)
            if (finite(in[i])) out[w++] = in[i];
        return w;
    }
    const std::size_t buckets = maxOut / 2;
    for (std::size_t b = 0; b < buckets; ++b) {
        const std::size_t lo = b * n / buckets, hi = (b + 1) * n / buckets;
        std::size_t imin = hi, imax = hi;
        for (std::size_t i = lo; i < hi; ++i) {
            if (!finite(in[i])) continue;
            if (imin == hi || in[i].y < in[imin].y) imin = i;
            if (imax == hi || in[i].y > in[imax].y) imax = i;
        }
        if (imin == hi) continue;
        if (imin == imax) {
            out[w++] = in[imin];
        } else if (imin < imax) {
            out[w++] = in[imin];
            out[w++] = in[imax];
        } else {
            out[w++] = in[imax];
            out[w++] = in[imin];
        }
    }
    return w;
}

double fitSlope(const PixelPoint* p, std::size_t n) noexcept {
    if (!p || n < 2) return std::numeric_limits<double>::quiet_NaN();
    double mx = 0, my = 0;
    for (std::size_t i = 0; i < n; ++i) { mx += p[i].x; my += p[i].y; }
    mx /= double(n); my /= double(n);
    double sxy = 0, sxx = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sxy += (p[i].x - mx) * (-(p[i].y - my));  // screen y is down; slope reported "up"
        sxx += (p[i].x - mx) * (p[i].x - mx);
    }
    if (sxx <= 0) return std::numeric_limits<double>::quiet_NaN();
    return sxy / sxx;
}

}  // namespace cd::analysis
