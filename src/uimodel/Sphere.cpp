// CircuitDrift uimodel: Tone State Sphere mapping. Owner: cd_uimodel. Render thread only.
#include "uimodel/Sphere.h"
#include <algorithm>
#include <cmath>

namespace cd::uimodel {

namespace {
double clamp01(double v) noexcept { return std::isfinite(v) ? std::fmin(1.0, std::fmax(0.0, v)) : 0.0; }
double lerp(double a, double b, double t) noexcept { return a + (b - a) * t; }
}  // namespace

double railFraction(double rail, double nominal) noexcept {
    if (!std::isfinite(rail) || !std::isfinite(nominal) || nominal <= 0) return 1.0;
    return clamp01(rail / nominal);
}
double squashOf(double r) noexcept { return 0.25 * (1.0 - clamp01(r)); }
double baseRadius(double rmsDbfs) noexcept {
    const double x = std::isfinite(rmsDbfs) ? std::fmin(60.0, std::fmax(0.0, rmsDbfs + 60.0)) : 0.0;
    return 0.85 + 0.10 * x / 60.0;
}

Rgb toneColor(double t) noexcept {
    t = clamp01(t);
    const Rgb cyan{0.0, 0.85, 0.95}, amber{1.0, 0.70, 0.15}, crimson{0.86, 0.08, 0.24};
    const Rgb &a = t < 0.5 ? cyan : amber, &b = t < 0.5 ? amber : crimson;
    const double u = t < 0.5 ? t * 2.0 : (t - 0.5) * 2.0;
    return {lerp(a.r, b.r, u), lerp(a.g, b.g, u), lerp(a.b, b.b, u)};
}

double edgeAmplitudeOf(double age, double drift, double radius) noexcept {
    return 0.03 * radius * clamp01(0.5 * clamp01(age) + 0.5 * clamp01(drift));
}

SphereVisual mapVisual(const ToneInputs& in) noexcept {
    SphereVisual v;
    v.silent = !std::isfinite(in.rmsDbfs) || in.rmsDbfs < kSilenceDbfs;
    v.railFraction = railFraction(in.railVolts, in.nominalRailVolts);
    v.sagIndicator = clamp01(in.sag);
    v.warmthIndicator = clamp01(in.warmth);
    v.controlPreview = !in.residualAvailable;
    v.colorT = in.residualAvailable ? clamp01(in.residualNorm) : clamp01(in.warmth);
    v.color = toneColor(v.colorT);
    const bool calm = v.silent || in.reduceMotion || !in.enabled;
    v.squash = calm ? 0.0 : squashOf(v.railFraction);
    v.radius = v.silent ? 0.85 : baseRadius(in.rmsDbfs);
    v.scaleV = verticalScale(v.squash);
    v.scaleH = horizontalScale(v.squash);
    v.edgeAmplitude = calm ? 0.0 : edgeAmplitudeOf(in.age, in.drift, v.radius);
    return v;
}

const SphereVisual& SphereSmoother::step(const SphereVisual& t, double dt) noexcept {
    if (!init_) { cur_ = t; init_ = true; return cur_; }
    const double a = (std::isfinite(dt) && dt > 0) ? 1.0 - std::exp(-dt / kSmoothingSeconds) : 0.0;
    auto s = [a](double& c, double target) { c += (target - c) * a; };
    s(cur_.railFraction, t.railFraction); s(cur_.squash, t.squash); s(cur_.radius, t.radius);
    s(cur_.scaleV, t.scaleV); s(cur_.scaleH, t.scaleH); s(cur_.colorT, t.colorT);
    s(cur_.edgeAmplitude, t.edgeAmplitude);
    s(cur_.sagIndicator, t.sagIndicator); s(cur_.warmthIndicator, t.warmthIndicator);
    cur_.color = toneColor(cur_.colorT);
    cur_.controlPreview = t.controlPreview;
    cur_.silent = t.silent;
    return cur_;
}

std::uint64_t RenderRng::next() noexcept {
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace cd::uimodel
