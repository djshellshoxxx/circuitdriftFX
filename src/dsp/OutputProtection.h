// CircuitDrift dsp: output protection + level-matched blend primitives (CD-F22, CD-F23).
// All functions noexcept, allocation-free, double precision. Sample-rate specific
// coefficients are computed in prepare-time helpers, never in the per-sample loop.
#pragma once
#include <algorithm>
#include <cmath>

namespace cd::dsp {

// Second-order Butterworth high-pass (Q=1/sqrt2), bilinear transform, TDF-II, a0-normalized.
struct ButterworthHpf {
    double b0 = 1, b1 = -2, b2 = 1, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    // Finite fs/fc must satisfy 0 < fc < fs/2; invalid/unrepresentable settings return false
    // without changing coefficients or delay state.
    bool design(double fs, double fc) noexcept;
    double process(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
};

// Memoryless output ceiling: identity to |x|<=0.9, then 0.9+0.1*tanh((|x|-0.9)/0.1);
// C1-continuous, |y|<=1 in floating point (tanh can round to exactly 1).
inline double softClip(double x) noexcept {
    const double a = std::fabs(x);
    if (a <= 0.9) return x;
    const double y = 0.9 + 0.1 * std::tanh((a - 0.9) / 0.1);
    return std::copysign(y, x);
}

// Linked-channel RMS match (01-runtime: 400 ms energy envelopes, 1 s log smoothing, 0.25..4 clamp,
// frozen below -60 dBFS RMS on either path).
class GainMatcher {
public:
    // A non-finite/non-positive sample rate is ignored without changing coefficients or state.
    void prepare(double fs) noexcept;
    void reset() noexcept;
    // Feed one aligned dry/wet mono sample pair; returns the wet multiplier.
    double update(double dry, double wet) noexcept;
    // Linked channels: feed the mean of their squared samples for each aligned dry/wet frame.
    // Non-finite/negative powers or overflowing updates are ignored without changing state.
    double updateMeanSquares(double dryPower, double wetPower) noexcept;
    double gain() const noexcept { return std::exp(logGain_); }
private:
    double aEnv_ = 0, aGain_ = 0, ed_ = 0, ew_ = 0, logGain_ = 0;
};

}  // namespace cd::dsp
