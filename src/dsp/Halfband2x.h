// CircuitDrift dsp: 2x halfband FIR up/down stage (CD-F21, T21).
// Kaiser-windowed sinc, cutoff fs_hi/4 (half-band: every even tap offset is zero except centre).
// Design target: passband ripple <=0.01 dB up to 0.2*fs_hi, stopband >=100 dB from 0.3*fs_hi.
// Threading: prepare() designs coefficients (not for audio thread); process calls are noexcept,
// allocation-free and bounded (kTaps MACs per high-rate sample). Double precision throughout.
// Latency: (kTaps-1)/2 high-rate samples per stage.
#pragma once
#include <array>
#include <cstddef>

namespace cd::dsp {

class Halfband2x {
public:
    static constexpr std::size_t kTaps = 77;
    static constexpr std::size_t kHalf = (kTaps - 1) / 2;  // 38

    // Computes the windowed-sinc coefficients. Not for the audio thread.
    void prepare() noexcept;
    void reset() noexcept;

    // Low-rate sample -> two high-rate samples (zero-stuffing, gain-compensated by 2).
    void upsample(double x, double& y0, double& y1) noexcept {
        y0 = filter(2.0 * x);
        y1 = filter(0.0);
    }

    // Two high-rate samples -> one low-rate sample (filter both, keep the output at the second).
    double downsample(double x0, double x1) noexcept {
        filter(x0);
        return filter(x1);
    }

    const std::array<double, kTaps>& coefficients() const noexcept { return h_; }

private:
    // Newest sample at index pos_; h_[k] multiplies the sample k steps back.
    double filter(double x) noexcept {
        z_[pos_] = x;
        double acc = 0.0;
        std::size_t idx = pos_;
        for (std::size_t k = 0; k < kTaps; ++k) {
            acc += h_[k] * z_[idx];
            idx = idx == 0 ? kTaps - 1 : idx - 1;
        }
        pos_ = pos_ + 1 == kTaps ? 0 : pos_ + 1;
        return acc;
    }

    std::array<double, kTaps> h_{};
    std::array<double, kTaps> z_{};
    std::size_t pos_ = 0;
};

}  // namespace cd::dsp
