#include "control/TempoClock.h"
#include <cmath>

namespace cd::control {

double TempoClock::beats(Division d, Modifier m, int num, int den) noexcept {
    if (num <= 0 || den <= 0 || num > 64 || den > 64) { num = 4; den = 4; }
    double q = 1.0;
    switch (d) {
        case Division::ThirtySecond: q = 0.125; break;
        case Division::Sixteenth: q = 0.25; break;
        case Division::Eighth: q = 0.5; break;
        case Division::Quarter: q = 1.0; break;
        case Division::Half: q = 2.0; break;
        case Division::Bar: q = num * 4.0 / den; break;
    }
    if (m == Modifier::Dotted) q *= 1.5;
    else if (m == Modifier::Triplet) q *= 2.0 / 3.0;
    return q;
}

double TempoClock::durationSeconds(double b, double bpm) noexcept { return 60.0 * b / bpm; }
double TempoClock::tauSeconds(double d) noexcept { return d / std::log(20.0); }
double TempoClock::coefficientFor(double tau, double fs) noexcept {
    if (!(tau > 0.0) || !(fs > 0.0)) return 0.0;
    return -std::expm1(-1.0 / (tau * fs));
}

bool TempoClock::prepare(double sampleRate) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 768000.0) return false;
    fs_ = sampleRate;
    ppq_ = 0.0;
    havePpq_ = false;
    status_ = TempoStatus{};
    coefTarget_ = 0.0;
    coef_ = 0.0;
    rampLeft_ = 0;
    retarget();
    return true;
}

void TempoClock::setDivision(Division d, Modifier m) noexcept {
    div_ = d;
    mod_ = m;
    retarget();
}

void TempoClock::retarget() noexcept {
    q_ = beats(div_, mod_, tsNum_, tsDen_);
    d_ = durationSeconds(q_, bpm_);
    tau_ = tauSeconds(d_);
    const double target = coefficientFor(tau_, fs_);
    if (target == coefTarget_) return;  // unchanged: do not restart the ramp (partition invariance)
    const bool first = (coefTarget_ == 0.0);
    coefTarget_ = target;
    const double n = std::floor(kCoefficientSmoothSeconds * fs_);
    if (!first && n >= 1.0) {
        rampLeft_ = static_cast<std::size_t>(n);
        coefInc_ = (coefTarget_ - coef_) / n;
    } else {
        coef_ = coefTarget_;
        rampLeft_ = 0;
    }
}

void TempoClock::setHost(const HostTempo& h) noexcept {
    if (!(fs_ > 0.0)) return;
    const bool bpmOk = h.bpmValid && std::isfinite(h.bpm) && h.bpm > 0.0;
    status_.bpmFallback = !bpmOk;
    status_.bpmClamped = false;
    double bpm = kFallbackBpm;
    if (bpmOk) {
        bpm = h.bpm;
        if (bpm < kMinBpm) { bpm = kMinBpm; status_.bpmClamped = true; }
        if (bpm > kMaxBpm) { bpm = kMaxBpm; status_.bpmClamped = true; }
    }
    bpm_ = bpm;
    const bool tsOk = h.timeSigValid && h.timeSigNum > 0 && h.timeSigDen > 0 && h.timeSigNum <= 64 && h.timeSigDen <= 64;
    status_.timeSigFallback = !tsOk;
    tsNum_ = tsOk ? h.timeSigNum : 4;
    tsDen_ = tsOk ? h.timeSigDen : 4;
    const bool ppqOk = h.ppqValid && std::isfinite(h.ppq) && std::fabs(h.ppq) < 1e9;
    status_.ppqFreeRun = !ppqOk;
    if (ppqOk && (!havePpq_ || h.transportJump || std::fabs(h.ppq - ppq_) > kPpqResyncTolerance)) {
        ppq_ = h.ppq;  // seek/loop or first sync: phase resets from PPQ
        havePpq_ = true;
    }
    retarget();
}

void TempoClock::step() noexcept {
    ppq_ += bpm_ / (60.0 * fs_);
    if (rampLeft_ > 0) {
        coef_ += coefInc_;
        if (--rampLeft_ == 0) coef_ = coefTarget_;
    }
}

void TempoClock::advance(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) step();
}

double TempoClock::phase() const noexcept {
    if (!(q_ > 0.0)) return 0.0;
    const double x = ppq_ / q_;
    double f = x - std::floor(x);
    if (f >= 1.0) f = 0.0;
    return f;
}

double TempoClock::drift(double depth) const noexcept {
    if (!std::isfinite(depth)) depth = 0.0;
    depth = depth < 0.0 ? 0.0 : (depth > 1.0 ? 1.0 : depth);
    return depth * std::sin(6.283185307179586 * phase());
}

}  // namespace cd::control
