// CircuitDrift dsp: coupled backward-Euler heat network (01-runtime-and-quality.md, T20).
// Units: C [J/K], G [W/K], P [W], T [K] absolute. dt [s].
// Threading: prepare() allocates nothing but is intended off-thread (O(N^3), N<=64);
// step() is noexcept, bounded, allocation-free and audio-thread safe.
// Error behavior: prepare() returns false (state untouched) on invalid C/G/connectivity or
// numerical overflow. State/step rejection retains all temperatures, never a partial update.
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

    // Every thermal island must contain a node with toAmbient > 0 (heat-sink contract).
    bool prepare(const Config& cfg, double dt, double ambientK) noexcept;
    // Reads size() temperatures in finite positive kelvin. Null, non-finite, non-positive or
    // numerically unrepresentable states are ignored atomically; no model upper bound is invented.
    bool isStateValid(const double* temperaturesK) const noexcept;
    bool setState(const double* temperaturesK) noexcept;
    // One implicit step with finite average power per node [W]. Null input, any overflow or a
    // non-positive candidate kelvin temperature leaves every temperature unchanged. Finite
    // negative power can represent cooling; physical calibration belongs to the model manifest.
    void step(const double* powerW) noexcept;
    double temperature(std::size_t i) const noexcept { return t_[i]; }
    std::size_t size() const noexcept { return n_; }

    // Ambient temperature for subsequent steps (matrix unchanged). Non-finite/non-positive or
    // numerically unrepresentable values are ignored (false).
    bool setAmbient(double ambientK) noexcept;
    double ambient() const noexcept { return ambient_; }
    // Thermal-mass control: every prepared heat capacity times s > 0, relative to prepare().
    // Refactors the constant matrix in place (O(n^3), bounded, allocation-free); the matrix is
    // strictly diagonally dominant so no pivoting is needed. Temperatures are kept. Invalid s or
    // a numerically unrepresentable result leaves the network unchanged (false).
    bool setCapacityScale(double s) noexcept;
    double capacityScale() const noexcept { return scale_; }

private:
    std::size_t n_ = 0;
    double ambient_ = 298.15, scale_ = 1.0;
    std::array<double, kMaxNodes> cOverDt_{}, baseCOverDt_{}, gAmb_{}, t_{};
    std::array<std::array<double, kMaxNodes>, kMaxNodes> inv_{};
    std::array<std::array<double, kMaxNodes>, kMaxNodes> gPart_{}, work_{};  // G_amb + Laplacian; scratch
};

}  // namespace cd::dsp
