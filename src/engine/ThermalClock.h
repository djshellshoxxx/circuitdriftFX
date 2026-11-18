// CircuitDrift engine: 1 kHz thermal clock (01-runtime coupled thermal contract, T20).
// Host samples accumulate dissipated power; a fractional phase accumulator (integer-valued
// increments of 1000 per sample against Fs, so integer rates tick exactly every Fs/1000 samples)
// triggers one backward-Euler ThermalNetwork step with the average power since the previous tick.
// Independent of block boundaries: state advances only per host sample. New temperatures affect
// the following samples (one-tick feedback lag, documented).
#pragma once
#include <algorithm>
#include <array>
#include "dsp/ThermalNetwork.h"

namespace cd::engine {

class ThermalClock {
public:
    static constexpr double kRateHz = 1000.0;
    bool prepare(const dsp::ThermalNetwork::Config& cfg, double fs, double ambientK) noexcept {
        if (!(fs > 0) || !net_.prepare(cfg, 1.0 / kRateHz, ambientK)) return false;
        fs_ = fs;
        n_ = cfg.n;
        clear();
        return true;
    }
    void clear() noexcept { acc_.fill(0.0); count_ = 0; phase_ = 0; }
    // Per host sample: add this sample's power [W] per node, then advance; true on a tick.
    void accumulate(const double* powerW) noexcept {
        for (std::size_t i = 0; i < n_; ++i) acc_[i] += powerW[i];
        ++count_;
    }
    bool advance() noexcept {
        phase_ += kRateHz;
        if (phase_ < fs_) return false;
        phase_ -= fs_;
        const double inv = count_ > 0 ? 1.0 / double(count_) : 0.0;
        std::array<double, dsp::ThermalNetwork::kMaxNodes> p{};
        for (std::size_t i = 0; i < n_; ++i) p[i] = acc_[i] * inv;
        net_.step(p.data());
        acc_.fill(0.0);
        count_ = 0;
        return true;
    }
    dsp::ThermalNetwork& network() noexcept { return net_; }
    const dsp::ThermalNetwork& network() const noexcept { return net_; }
    std::size_t size() const noexcept { return n_; }
    // Checkpoint access.
    double& phase() noexcept { return phase_; }
    double& count() noexcept { return count_; }
    double* accumulator() noexcept { return acc_.data(); }
private:
    dsp::ThermalNetwork net_;
    std::array<double, dsp::ThermalNetwork::kMaxNodes> acc_{};
    double fs_ = 48000, phase_ = 0, count_ = 0;
    std::size_t n_ = 0;
};

}  // namespace cd::engine
