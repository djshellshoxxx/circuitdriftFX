// CircuitDrift uimodel: UX-07 Tone State Sphere visual mapping (pure functions + 80 ms smoother).
// Owner: cd_uimodel. Framework-independent. The sphere is an expressive summary, not a calibrated
// scope. Threading: message/render thread only. The render RNG is a private generator that has no
// connection to any DSP RNG (nothing here includes runtime or dsp headers).
#pragma once
#include <cstdint>

namespace cd::uimodel {

struct ToneInputs {
    double railVolts = 0, nominalRailVolts = 0;   // selected rail and V_nom
    double rmsDbfs = -120;
    bool residualAvailable = false;               // calibrated linear-reference residual measured
    double residualNorm = 0;                      // normalized nonlinear residual energy 0..1
    double warmth = 0, age = 0, drift = 0, sag = 0;   // parameters 0..1
    bool reduceMotion = false;
    bool enabled = true;                          // animate only while enabled
};

struct Rgb { double r = 0, g = 0, b = 0; };       // 0..1

struct SphereVisual {
    double railFraction = 1, squash = 0;
    double radius = 0.85, scaleV = 1, scaleH = 1;
    Rgb color{};
    double colorT = 0;                            // 0 cyan .. 0.5 amber .. 1 crimson
    bool controlPreview = false;                  // color from Warmth parameter, not measured distortion
    double edgeAmplitude = 0;                     // fraction of radius, <= 0.03
    bool silent = false;                          // calm ring
    double sagIndicator = 0, warmthIndicator = 0; // numeric indicators stay with Reduce Motion
};

inline constexpr double kSilenceDbfs = -80.0;     // below this RMS the sphere holds a calm ring
inline constexpr double kSmoothingSeconds = 0.080;

double railFraction(double rail, double nominal) noexcept;              // clamp(rail/V_nom,0,1); invalid V_nom -> 1
double squashOf(double railFraction) noexcept;                          // 0.25*(1-r)
double baseRadius(double rmsDbfs) noexcept;                             // 0.85+0.10*clamp(rms+60,0,60)/60
inline double verticalScale(double squash) noexcept { return 1.0 - squash; }
inline double horizontalScale(double squash) noexcept { return 1.0 + 0.5 * squash; }
Rgb toneColor(double t) noexcept;                                       // cyan->amber->crimson, t clamped
double edgeAmplitudeOf(double age, double drift, double radius) noexcept;   // <= 0.03*radius
SphereVisual mapVisual(const ToneInputs& in) noexcept;                  // unsmoothed target

// One-pole smoother (tau 80 ms): alpha = 1-exp(-dt/tau). Silence and Reduce Motion drive the
// deformation terms to calm through the same smoothing, never by jumping.
class SphereSmoother {
public:
    void reset(const SphereVisual& v) noexcept { cur_ = v; init_ = true; }
    const SphereVisual& step(const SphereVisual& target, double dtSeconds) noexcept;
    const SphereVisual& current() const noexcept { return cur_; }
private:
    SphereVisual cur_{};
    bool init_ = false;
};

// Render-only RNG (splitmix64), seeded independently of the audio RNG.
class RenderRng {
public:
    explicit RenderRng(std::uint64_t seed) noexcept : s_(seed) {}
    std::uint64_t next() noexcept;
    double uniform() noexcept { return double(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0,1)
    double signedUnit() noexcept { return 2.0 * uniform() - 1.0; }
private:
    std::uint64_t s_;
};

}  // namespace cd::uimodel
