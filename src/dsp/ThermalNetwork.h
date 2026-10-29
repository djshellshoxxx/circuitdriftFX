// CircuitDrift dsp: coupled backward-Euler heat network (01-runtime-and-quality.md, T20).
// Units: C [J/K], G [W/K], P [W], T [K] absolute. dt [s].
// Threading: prepare() allocates nothing but is intended off-thread (O(N^3), N<=64);
// step() is noexcept, bounded, allocation-free and audio-thread safe.
// Error behavior: prepare() returns false (state untouched) on invalid C/G/connectivity.
#pragma once
#include <array>
#include <cstddef>

namespace cd::dsp {

class ThermalNetwork {
public:
    static constexpr std::size_t kMaxNodes = 64;

    struct Config {
        std::size_t n = 0;
        std::array<double, kMaxNodes> capacity{};        // C_i > 0
        std::array<double, kMaxNodes> toAmbient{};       // G_i,amb >= 0
        std::array<std::array<double, kMaxNodes>, kMaxNodes> coupling{};  // symmetric G_ij >= 0, zero diagonal
    };

    // Every thermal island must contain a node with toAmbient > 0; otherwise A is singular.
    bool prepare(const Config& cfg, double dt, double ambientK) noexcept;
    void setState(const double* temperaturesK) noexcept;
    // One implicit step with average dissipated power per node [W].
    void step(const double* powerW) noexcept;
    double temperature(std::size_t i) const noexcept { return t_[i]; }
    std::size_t size() const noexcept { return n_; }

private:
    std::size_t n_ = 0;
    double ambient_ = 298.15;
    std::array<double, kMaxNodes> cOverDt_{}, gAmb_{}, t_{};
    std::array<std::array<double, kMaxNodes>, kMaxNodes> inv_{};
};

}  // namespace cd::dsp
