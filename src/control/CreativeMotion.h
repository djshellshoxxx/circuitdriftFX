// Seeded creative motion (P05; spec "Actual thermal state versus creative stochastic drift").
// Owner: cd_control. Threading: prepare() allocates (control thread); all other calls are audio-safe.
// NOT physical temperature: a labelled creative control process. Not claimed to be exact pink noise.
// PRNG: SplitMix64 + xoshiro256** (Blackman & Vigna, https://prng.di.unimi.it/, public domain/CC0).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cd::control {

enum class StreamType : uint32_t { CutoffMotion = 1, DelayMotion = 2, Other = 3 };

uint64_t splitmix64(uint64_t& state) noexcept;
// Deterministic seed from (instanceSeed, slot, componentUuid text, streamType) via splitmix hashing.
uint64_t deriveSeed(uint64_t instanceSeed, uint32_t slot, const char* componentUuid, StreamType type) noexcept;

class Xoshiro256ss {
public:
    void seed(uint64_t s) noexcept;
    uint64_t next() noexcept;
    double uniform01Open() noexcept;   // (0,1]
    double gaussian() noexcept;        // Box-Muller, caches the second value
private:
    uint64_t s_[4] = {1, 2, 3, 4};
    double spare_ = 0; bool hasSpare_ = false;
};

// Three OU processes on a 100 Hz control clock, tau {0.2,2,20} s; q = tanh(sum/sqrt3) in (-1,1).
// Advance is per audio sample with a fractional phase accumulator, so block partition is irrelevant.
// q is zero-order held between control ticks (consumers slew it).
class CreativeMotion {
public:
    bool prepare(double sampleRate, uint64_t seed) noexcept;
    void advanceSample() noexcept;
    void advance(std::size_t n) noexcept { while (n--) advanceSample(); }
    double q() const noexcept { return q_; }
private:
    void tick() noexcept;
    Xoshiro256ss rng_;
    double u_[3] = {0, 0, 0}, a_[3] = {0, 0, 0}, s_[3] = {0, 0, 0};
    double phase_ = 0, inc_ = 0, q_ = 0;
};

// Pitch-drift delay: 8 ms ring allocated in prepare, 4-point Lagrange (cubic) interpolation,
// base delay 1.5 ms, deviation 1.5*d*q ms, slew 0.002 s/s. Creative effect, disabled in Zero Latency.
class PitchDriftDelay {
public:
    static constexpr double kBaseMs = 1.5, kDevMs = 1.5, kSlew = 0.002;
    bool prepare(double sampleRate) noexcept;   // allocates
    void setEnabled(bool e) noexcept { enabled_ = e; }
    void reset() noexcept;
    double process(double x, double d, double q) noexcept;
    double latencySamples() const noexcept { return enabled_ ? kBaseMs * 1e-3 * fs_ : 0.0; }  // base delay
    double currentDelaySamples() const noexcept { return cur_; }
    bool enabled() const noexcept { return enabled_; }
    // Snapshot support (audio-safe, allocation-free). stateSize() doubles: ring position, current
    // delay, then the ring contents. validState() checks finiteness and bounds against this prepared
    // size; setState() applies only a valid state (all-or-nothing). Sizes must match (same Fs).
    std::size_t stateSize() const noexcept { return ring_.empty() ? 0 : ring_.size() + 2; }
    void getState(double* out) const noexcept;
    bool validState(const double* in) const noexcept;
    bool setState(const double* in) noexcept;
private:
    std::vector<double> ring_;
    std::size_t mask_ = 0, pos_ = 0;
    double fs_ = 0, cur_ = 0;
    bool enabled_ = true;
};

}  // namespace cd::control
