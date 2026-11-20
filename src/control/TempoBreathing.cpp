#include "control/TempoBreathing.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace cd::control {
namespace {
double frac(double x) noexcept { return x - std::floor(x); }
double wrapHalf(double x) noexcept { return x - std::floor(x + 0.5); }  // [-0.5,0.5)
}

double TempoBreathing::clampBpm(double bpm) noexcept {
    if (!std::isfinite(bpm)) return 120.0;
    return std::clamp(bpm, 20.0, 300.0);
}

double TempoBreathing::beatsPerCycle(int division, int modifier, int tsNum, int tsDen) noexcept {
    static constexpr double kQ[5] = {0.125, 0.25, 0.5, 1.0, 2.0};  // 1/32..1/2 in quarter notes
    division = std::clamp(division, 0, 5);
    double q;
    if (division < 5) {
        q = kQ[division];
    } else {
        const int num = tsNum > 0 ? tsNum : 4, den = tsDen > 0 ? tsDen : 4;
        q = double(num) * 4.0 / double(den);
    }
    if (modifier == 1) q *= 1.5;
    else if (modifier == 2) q *= 2.0 / 3.0;
    return q;
}

double TempoBreathing::recoveryTau(double q, double bpm) noexcept {
    return (60.0 * q / clampBpm(bpm)) / std::log(20.0);
}

void TempoBreathing::prepare(double sampleRate) noexcept {
    fs_ = sampleRate > 0 ? sampleRate : 48000.0;
    reset();
}

void TempoBreathing::reset() noexcept {
    phi_ = phaseErr_ = tauSm_ = lastPpqEnd_ = lastQ_ = 0;
    wasEnabled_ = havePpq_ = false;
    out_ = TempoOutput{};
}

TempoOutput TempoBreathing::process(const TempoInput& in, int frames) noexcept {
    const bool bpmKnown = in.bpm.has_value() && std::isfinite(*in.bpm);
    const double bpm = bpmKnown ? clampBpm(*in.bpm) : 120.0;
    const double q = beatsPerCycle(in.division, in.modifier, in.timeSigNum, in.timeSigDen);
    const double tauTarget = recoveryTau(q, bpm);
    const double dt = frames > 0 ? double(frames) / fs_ : 0.0;
    const double bps = bpm / 60.0;

    out_.fallback = in.enabled && !bpmKnown;
    out_.bpmUsed = bpm;
    out_.beatsPerCycle = q;

    if (!in.enabled) {
        wasEnabled_ = false;
        out_.recoveryTauS = 0;
        out_.cutoffOct = 0;
        return out_;
    }
    if (!wasEnabled_) {  // snap on enable: no ramp from a stale value
        tauSm_ = tauTarget;
        wasEnabled_ = true;
        lastQ_ = q;
    }
    if (frames <= 0) {  // no clock advance; parameters still reflected in the immediate outputs
        out_.recoveryTauS = tauSm_;
        out_.cutoffOct = std::clamp(in.depth, 0.0, 1.0) * kMaxCutoffOct *
                         std::sin(2.0 * std::numbers::pi * (phi_ + phaseErr_));
        out_.phase = frac(phi_ + phaseErr_);
        return out_;
    }

    tauSm_ += (tauTarget - tauSm_) * (1.0 - std::exp(-dt / kTauSmoothS));

    const double advanceBeats = in.playing ? bps * dt : 0.0;
    if (in.ppq.has_value() && std::isfinite(*in.ppq)) {
        const double ppq = *in.ppq;
        const double endBeats = ppq + advanceBeats;
        // Reduce in beat units first: a finite PPQ divided by a short cycle can overflow.
        const double newPhi = frac(std::fmod(endBeats, q) / q);
        // Reference the previous effective phase at this block's start to detect a jump.
        const bool seek = !havePpq_ || std::fabs(ppq - lastPpqEnd_) > 0.01 || std::fabs(q - lastQ_) > 1e-12;
        if (seek) {
            // Carry the old effective phase forward by this block's advance, keep the error to decay.
            const double oldEnd = (phi_ + phaseErr_) + (in.playing ? bps * dt / q : 0.0);
            phaseErr_ = havePpq_ || phi_ != 0.0 ? wrapHalf(oldEnd - newPhi) : 0.0;
            // first PPQ ever seen after a free-run start also takes the smoothed path
        }
        phi_ = newPhi;
        havePpq_ = true;
        lastPpqEnd_ = endBeats;
    } else {
        phi_ = frac(phi_ + advanceBeats / q);
        havePpq_ = false;
    }
    lastQ_ = q;
    phaseErr_ *= std::exp(-dt / kPhaseSmoothS);

    const double ph = phi_ + phaseErr_;
    out_.recoveryTauS = tauSm_;
    out_.phase = frac(ph);
    out_.cutoffOct = std::clamp(in.depth, 0.0, 1.0) * kMaxCutoffOct * std::sin(2.0 * std::numbers::pi * ph);
    return out_;
}

}  // namespace cd::control
