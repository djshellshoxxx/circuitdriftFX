// CircuitDrift dsp: coupled backward-Euler heat network (01-runtime-and-quality.md, T20).
// Units: C [J/K], G [W/K], P [W], T [K] absolute. dt [s].
// Threading: prepare() allocates nothing but is intended off-thread (O(N^3), N<=64);
// step() is noexcept, bounded, allocation-free and audio-thread safe.
// Error behavior: prepare() returns false (state untouched) on invalid C/G/connectivity or
// numerical overflow. State/step rejection retains all temperatures, never a partial update.
// Diagnostics: every step() records a bounded StepStatus and saturating accepted/rejected
// counters (audio-thread safe, noexcept, no allocation). A successful prepare() resets them.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace cd::dsp {

class ThermalNetwork {
public:
    static constexpr std::size_t kMaxNodes = 64;

    // Outcome of the most recent step(). Every value except Accepted means no temperature changed.
    enum class StepStatus : unsigned char {
        None,                    // no step since construction/prepare/clearStepDiagnostics
        Accepted,                // candidate committed
        NotPrepared,             // no successful prepare(); nothing to integrate
        NullInput,               // powerW == nullptr
        NonFinitePower,          // some powerW[i] is NaN/Inf
        Overflow,                // right-hand side, solve or next-step input became non-finite
        NonPositiveTemperature,  // some candidate kelvin temperature <= 0
    };

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
    void setState(const double* temperaturesK) noexcept;
    // One implicit step with finite average power per node [W]. Null input, any overflow or a
    // non-positive candidate kelvin temperature leaves every temperature unchanged. Finite
    // negative power can represent cooling; physical calibration belongs to the model manifest.
    void step(const double* powerW) noexcept;
    double temperature(std::size_t i) const noexcept { return t_[i]; }
    std::size_t size() const noexcept { return n_; }

    // Step diagnostics (noexcept, audio-thread safe). Counters saturate at UINT64_MAX.
    StepStatus lastStepStatus() const noexcept { return lastStatus_; }
    std::uint64_t acceptedSteps() const noexcept { return accepted_; }
    std::uint64_t rejectedSteps() const noexcept { return rejected_; }
    std::uint64_t consecutiveRejectedSteps() const noexcept { return consecutiveRejected_; }
    void clearStepDiagnostics() noexcept;

private:
    StepStatus integrate(const double* powerW) noexcept;
    StepStatus lastStatus_ = StepStatus::None;
    std::uint64_t accepted_ = 0, rejected_ = 0, consecutiveRejected_ = 0;
    std::size_t n_ = 0;
    double ambient_ = 298.15;
    std::array<double, kMaxNodes> cOverDt_{}, gAmb_{}, t_{};
    std::array<std::array<double, kMaxNodes>, kMaxNodes> inv_{};
};

}  // namespace cd::dsp
