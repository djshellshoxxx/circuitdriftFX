// cd_control: seeded creative stochastic drift (02-musical-controls "Actual thermal state versus creative
// stochastic drift"). CREATIVE modulation, not physics: it never feeds kelvin or any electrical state.
//
// Three stationary AR(1) processes u_j[k+1]=a_j u_j[k]+sqrt(1-a_j^2) z_j[k], tau={0.2,2,20} s,
// a_j=exp(-0.01/tau_j), on a 100 Hz control clock (integer tick count floor(n*100/fs)). Exact integer
// phase arithmetic keeps the sequence independent of block partitioning, including large counters.
// The sample clock saturates at UINT64_MAX. q=tanh(sum/sqrt3).
// Delay deviation is slew limited (0.002 s/s = 2 ms/s) per control tick, before depth scaling.
//
// Threading: prepare() off the audio thread; advance()/accessors noexcept, no allocation.
// Checkpoint: State is POD; save()/restore() reproduce the future output exactly.
#pragma once
#include <cstdint>
#include "control/Random.h"

namespace cd::control {

class CreativeDrift {
public:
    static constexpr double kTau[3] = {0.2, 2.0, 20.0};
    static constexpr double kClockHz = 100.0;
    static constexpr double kMaxCutoffOct = 0.15;   // at depth 1
    static constexpr double kMaxDelayMs = 1.5;      // at depth 1
    static constexpr double kSlewMsPerS = 2.0;      // 0.002 s/s

    struct State {
        RngState rng[3]{};
        double u[3] = {0, 0, 0};
        double q = 0;
        double delayUnitMs = 0;   // slewed 1.5*q (depth 1), before depth scaling
        uint64_t samples = 0;
        uint64_t ticks = 0;
    };

    // Seed derived from (instanceSeed, slot, stream j). Draws the stationary initial state.
    // Supported sample rates are finite 8000..768000 Hz; other values use 48000 Hz.
    void prepare(double sampleRate, uint64_t instanceSeed, uint32_t slot) noexcept;
    void advance(int frames) noexcept;  // frames==0 advances nothing

    double q() const noexcept { return st_.q; }
    double cutoffOct(double depth) const noexcept { return kMaxCutoffOct * depth * st_.q; }
    double delayMs(double depth) const noexcept { return depth * st_.delayUnitMs; }

    State save() const noexcept {
        State s = st_;
        for (int j = 0; j < 3; ++j) s.rng[j] = rng_[j].state();
        return s;
    }
    void restore(const State& s) noexcept;  // incompatible/invalid snapshots leave the current state intact
    uint64_t ticks() const noexcept { return st_.ticks; }

private:
    void tick() noexcept;
    uint64_t clockPosition(uint64_t samples, uint64_t& phase) const noexcept;
    // Reduced exact ratio 100/fs = clockStep_/clockPeriod_. Each term is below 2^53.
    uint64_t clockPeriod_ = 480, clockStep_ = 1, samplePhase_ = 0;
    GaussianRng rng_[3];
    State st_{};
};

}  // namespace cd::control
