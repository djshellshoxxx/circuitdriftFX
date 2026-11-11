#include "control/Macros.h"
#include "core/ParameterRegistry.h"
#include <algorithm>
#include <cmath>

namespace cd::control {
namespace {
double clamp01(double x) noexcept { return std::isfinite(x) ? std::clamp(x, 0.0, 1.0) : 0.0; }
double fin(double x, double d) noexcept { return std::isfinite(x) ? x : d; }
constexpr double kGlobalTolSigma = 0.25;  // global +-25% envelope

// Manifest-derived numbers are validated at the boundary: a macro whose bindings are not finite
// or not ordered is unsupported (neutral) rather than propagating NaN/inverted ranges.
struct Usable { bool age, sag, warmth, drift; double maxTol; };
Usable usable(const ManifestBindings& mb) noexcept {
    const bool tolOk = std::isfinite(mb.maxTolSigma) && mb.maxTolSigma >= 0.0;
    Usable u{};
    u.maxTol = tolOk ? std::min(mb.maxTolSigma, kGlobalTolSigma) : 0.0;
    u.age = mb.hasAge && tolOk && std::isfinite(mb.leakRange) && mb.leakRange >= 0.0;
    u.sag = mb.hasSag && std::isfinite(mb.cBase) && std::isfinite(mb.cMin) && mb.cMin > 0.0 && mb.cBase >= mb.cMin;
    u.warmth = mb.hasWarmth && std::isfinite(mb.biasSpan) && mb.biasSpan >= 0.0 && std::isfinite(mb.fluxMin) &&
               std::isfinite(mb.fluxMax) && mb.fluxMin <= mb.fluxMax;
    u.drift = mb.hasDrift;
    return u;
}
}

Effective evaluate(const BaseValues& base, const MacroValues& mv, const ManifestBindings& mbIn, const Hooks& h) noexcept {
    Effective e;
    const Usable us = usable(mbIn);
    ManifestBindings mb = mbIn;
    mb.hasAge = us.age; mb.hasSag = us.sag; mb.hasWarmth = us.warmth; mb.hasDrift = us.drift;
    // base -> timeline offsets
    const TimelineOffsets tl = h.timeline ? *h.timeline : TimelineOffsets{};
    e.serviceAgeYears = fin(base.serviceAgeYears, 0) + fin(tl.serviceAgeYears, 0);
    e.tolSigma = fin(base.tolSigma, 0) + fin(tl.tolSigma, 0);
    e.leakFrac = fin(base.leakFrac, 0) + fin(tl.leakFrac, 0);
    e.driveDb = fin(base.driveDb, 0) + fin(tl.driveDb, 0);
    e.biasOffset = fin(base.biasOffset, 0) + fin(tl.biasOffset, 0);
    e.fluxFactor = fin(base.fluxFactor, 1);
    e.driftSensitivity = fin(base.driftSensitivity, 1);
    e.loadMultiplier = fin(base.loadMultiplier, 1);
    e.reservoirC = (us.sag || !mbIn.hasSag) ? fin(mbIn.cBase, 0) : 0.0;
    // macros (missing binding -> unsupported, neutral)
    const double a = clamp01(mv.age), s = clamp01(mv.sag), d = clamp01(mv.drift), w = clamp01(mv.warmth);
    const double q = h.mod ? std::clamp(fin(h.mod->q, 0), -1.0, 1.0) : 0.0;
    e.supportsAge = mb.hasAge; e.supportsSag = mb.hasSag; e.supportsWarmth = mb.hasWarmth; e.supportsDrift = mb.hasDrift;
    if (mb.hasAge) {
        e.serviceAgeYears = std::max(e.serviceAgeYears, 60.0 * a);
        e.tolSigma += 0.08 * a;
        e.leakFrac += 0.1 * a * a * mb.leakRange;
        e.shelfGainDb = -6.0 * a;
        e.noiseEnabled = mb.analogNoise;
        e.noiseDbfs = -90.0 + 24.0 * a;
    }
    if (mb.hasSag) {
        e.reservoirC = std::max(mb.cMin, mb.cBase / (1.0 + 3.0 * s));
        e.loadMultiplier *= 1.0 + 3.0 * s;
    }
    if (mb.hasWarmth) {
        e.driveDb += 18.0 * w;
        e.biasOffset += 0.08 * w * mb.biasSpan;
        e.fluxFactor *= 1.0 + 0.5 * w;
    }
    if (mb.hasDrift) {  // modulation stage: creative motion q scaled by d
        e.driftSensitivity *= 1.0 + 4.0 * d;
        e.cutoffMotionOct = 0.15 * d * q;
        e.delayDevMs = 1.5 * d * q;
    }
    // faults
    if (h.fault) {
        e.leakFrac += fin(h.fault->leakFracAdd, 0);
        e.loadMultiplier *= std::max(0.0, fin(h.fault->loadMultiplierScale, 1));
        e.biasOffset += fin(h.fault->biasOffsetAdd, 0);
    }
    // domain clamp
    e.serviceAgeYears = std::max(0.0, e.serviceAgeYears);
    e.tolSigma = std::clamp(e.tolSigma, 0.0, us.maxTol);
    e.leakFrac = std::clamp(e.leakFrac, 0.0, 1.0);
    if (std::isfinite(mb.biasSpan) && mb.biasSpan > 0) e.biasOffset = std::clamp(e.biasOffset, -mb.biasSpan, mb.biasSpan);
    if (mb.hasWarmth) e.fluxFactor = std::clamp(e.fluxFactor, mb.fluxMin, mb.fluxMax);
    e.loadMultiplier = std::max(0.0, e.loadMultiplier);
    return e;
}

double composeLoadCurrent(SidechainSource src, double iPhys, double iDyn, double sag, double iExt) noexcept {
    iPhys = fin(iPhys, 0); iDyn = std::max(0.0, fin(iDyn, 0)); iExt = std::max(0.0, fin(iExt, 0));
    const double s = clamp01(sag);
    switch (src) {
        case SidechainSource::Main: return iPhys + 3.0 * s * iDyn;
        case SidechainSource::External: return iPhys + iExt;
        case SidechainSource::Blend: return iPhys + 3.0 * s * iDyn + iExt;
    }
    return iPhys;
}

// RBJ "Cookbook formulae for audio EQ biquad filter coefficients" (Robert Bristow-Johnson), highShelf:
// A=10^(dB/40), w0=2 pi f0/fs, alpha=sin(w0)/(2Q), r=2 sqrt(A) alpha
// b0=A((A+1)+(A-1)cos+r) b1=-2A((A-1)+(A+1)cos) b2=A((A+1)+(A-1)cos-r)
// a0=(A+1)-(A-1)cos+r a1=2((A-1)-(A+1)cos) a2=(A+1)-(A-1)cos-r
BiquadCoeffs designHighShelf(double fs, double f0, double gainDb, double q) noexcept {
    BiquadCoeffs c;
    if (!(fs > 0) || !(f0 > 0) || !(f0 < 0.5 * fs) || !(q > 0) || !std::isfinite(gainDb)) return c;
    const double A = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * M_PI * f0 / fs;
    const double cs = std::cos(w0), al = std::sin(w0) / (2.0 * q), r = 2.0 * std::sqrt(A) * al;
    const double a0 = (A + 1) - (A - 1) * cs + r;
    c.b0 = A * ((A + 1) + (A - 1) * cs + r) / a0;
    c.b1 = -2.0 * A * ((A - 1) + (A + 1) * cs) / a0;
    c.b2 = A * ((A + 1) + (A - 1) * cs - r) / a0;
    c.a1 = 2.0 * ((A - 1) - (A + 1) * cs) / a0;
    c.a2 = ((A + 1) - (A - 1) * cs - r) / a0;
    return c;
}

void MacroSmoother::prepare(double fs, const double* init, std::size_t n, double tau) noexcept {
    n_ = std::min(n, kMax);
    a_ = core::smoothingCoeff(fs, tau);
    for (std::size_t i = 0; i < kMax; ++i) y_[i] = t_[i] = (init && i < n_ && std::isfinite(init[i])) ? init[i] : 0.0;
}
void MacroSmoother::setTargets(const double* t, std::size_t n) noexcept {
    for (std::size_t i = 0; t && i < std::min(n, n_); ++i) if (std::isfinite(t[i])) t_[i] = t[i];
}
void MacroSmoother::step() noexcept {
    for (std::size_t i = 0; i < n_; ++i) y_[i] = t_[i] + a_ * (y_[i] - t_[i]);
}

}  // namespace cd::control
