// CircuitDrift runtime: small preallocated helpers shared by CircuitRuntime, OutputStage and Engine.
// Owner: cd_runtime (runtime agent). Contents: integer delay line, linear ramp, one-pole smoother
// and the declared-latency plan (01-runtime "Oversampling and quality": pad within one phase
// family to the maximum integer latency among 1-16x, compensate fractional delay explicitly).
// Threading: prepare()/planLatency() allocate and run off the audio thread; every other member
// is noexcept, bounded and allocation-free.
#pragma once
#include <cstdint>
#include <vector>
#include "dsp/Oversampling.h"

namespace cd::runtime {

// Fixed integer delay (0 = pass-through). Ring storage is sized once in prepare().
class DelayLine {
public:
    void prepare(int delaySamples);           // allocates; delaySamples >= 0
    double process(double x) noexcept {
        if (d_ == 0) return x;
        const double y = buf_[pos_];
        buf_[pos_] = x;
        if (++pos_ == d_) pos_ = 0;
        return y;
    }
    void reset() noexcept;
    int delay() const noexcept { return d_; }
private:
    std::vector<double> buf_;
    int d_ = 0, pos_ = 0;
};

// Linear ramp that lands exactly on its target after a fixed number of samples.
class LinearRamp {
public:
    void prepare(int rampSamples) noexcept { n_ = rampSamples < 1 ? 1 : rampSamples; }
    void snap(double v) noexcept { value_ = target_ = v; left_ = 0; }
    void setTarget(double t) noexcept {
        if (t == target_) return;
        target_ = t; left_ = n_; step_ = (target_ - value_) / double(n_);
    }
    double next() noexcept {  // returns the value for the current sample, then advances
        if (left_ > 0) { value_ = (--left_ == 0) ? target_ : value_ + step_; }
        return value_;
    }
    double value() const noexcept { return value_; }
    double target() const noexcept { return target_; }
    bool ramping() const noexcept { return left_ > 0; }
private:
    double value_ = 0, target_ = 0, step_ = 0;
    int n_ = 1, left_ = 0;
};

// One-pole exponential smoother with time constant tau; snaps when within 1e-12 relative of target
// so settled parameters become exact (bit-identical comparisons across partitions still hold since
// the update is per sample).
class OnePole {
public:
    void prepare(double fs, double tauSec) noexcept;
    void snap(double v) noexcept { y_ = target_ = v; }
    void setTarget(double t) noexcept { target_ = t; }
    double next() noexcept {
        if (y_ != target_) {
            y_ = target_ + a_ * (y_ - target_);
            const double tol = 1e-12 * (target_ < 0 ? -target_ : target_) + 1e-15;
            if ((y_ - target_ <= tol) && (target_ - y_ <= tol)) y_ = target_;
        }
        return y_;
    }
    double value() const noexcept { return y_; }
    double target() const noexcept { return target_; }
private:
    double a_ = 0, y_ = 0, target_ = 0;
};

// Declared latency of one oversampled stage.
// osLatency: Oversampler::latency() in base samples (FIR exact, IIR nominal DC group delay).
// declared:  integer base-sample latency reported for this stage.
// padHigh:   extra integer delay, in HIGH-rate samples, inserted inside the oversampled domain so
//            that osLatency + padHigh/factor == declared. FIR round-trip latency is always an
//            integer number of high-rate samples, so FIR compensation is exact (residual 0) and
//            lossless (no interpolation). IIR residual is |declared - osLatency - padHigh/F| <=
//            0.5/F base samples at DC only (nominal; IIR phase is frequency dependent).
struct LatencyPlan {
    double osLatency = 0;
    int declared = 0;
    int padHigh = 0;
    double residual = 0;
    double familyMax = 0;   // max latency among 1/2/4/8/16x of this family (when padding)
};

// padToFamilyMax: true for normal modes (pad to max among 1-16x of spec.family at the same pass/
// stop design), false for explicit Zero Latency / unpadded diagnostics. Returns false if any
// required Oversampler design fails. Allocates temporary designs (off-thread only).
bool planLatency(const dsp::Oversampler::Spec& spec, bool padToFamilyMax, LatencyPlan& out);

inline bool isFiniteSample(double x) noexcept { return x - x == 0.0; }

}  // namespace cd::runtime
