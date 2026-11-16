// CircuitDrift analysis: thermal/XY scope math (UX-04).
// Owner: cd_analysis. Pure functions, UI/worker thread, no allocation in mapping/decimation
// (caller supplies output storage). Equal volts-per-pixel scaling on both axes so a unity-gain
// linear reference is a 45-degree line; mode availability is honest (B-H needs H and B fields).
#pragma once
#include "analysis/Telemetry.h"
#include <cstddef>
#include <cstdint>

namespace cd::analysis {

enum class ScopeMode { TransferXY, VoltageTemperature, VoltageRail, MagneticBH };

struct ScopeModeInfo {
    bool available = false;
    const char* xLabel = "";
    const char* yLabel = "";
    const char* unavailableReason = "";
};
// magneticFieldsPresent: the selected model supplies H [A/m] and B [T].
ScopeModeInfo scopeModeInfo(ScopeMode mode, bool magneticFieldsPresent, bool railIsFraction = false) noexcept;

struct PixelPoint {
    double x = 0.0, y = 0.0;
};

// Fixed-scale mapping: one voltsPerPixel for both axes; origin (0,0) at pixel (cx, cy); y up.
class XyMapper {
public:
    // Invalid parameters (non-finite, voltsPerPixel<=0, sizes<=0) leave valid()==false.
    XyMapper(int widthPx, int heightPx, double voltsPerPixel) noexcept;
    bool valid() const noexcept { return valid_; }
    // Exact (subpixel) position; non-finite input returns {NaN,NaN}.
    PixelPoint map(double xVolts, double yVolts) const noexcept;
    // Pixel-center rasterisation (nearest pixel).
    PixelPoint mapRounded(double xVolts, double yVolts) const noexcept;
    bool onScreen(PixelPoint p) const noexcept;

private:
    double cx_ = 0, cy_ = 0, inv_ = 0;
    int w_ = 0, h_ = 0;
    bool valid_ = false;
};

// Ordered extrema-preserving decimation: splits n points into maxOut/2 contiguous buckets and emits
// each bucket's min-y and max-y points in original time order (no sorting by amplitude). If
// n <= maxOut the points are copied unchanged. Returns points written (<= maxOut). Non-finite
// points are skipped. maxOut is clamped to >= 2.
std::size_t decimateExtrema(const XyPair* in, std::size_t n, XyPair* out, std::size_t maxOut) noexcept;

inline constexpr std::size_t kMaxScopeTracePoints = 2048;

// Least-squares slope dy/dx of mapped pixel points (screen-space y flipped back to "up").
double fitSlope(const PixelPoint* pts, std::size_t n) noexcept;

}  // namespace cd::analysis
