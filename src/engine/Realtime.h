// CircuitDrift engine: small realtime building blocks (smoothers, biquads, delay lines, RNG,
// lock-free triple buffer). All per-sample methods are noexcept, allocation-free and double
// precision. Storage is sized in prepare-time code (allocating), never on the audio thread.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace cd::engine {

constexpr double kPi = std::numbers::pi;

// One-pole exponential smoother y += (1-a)(target-y); a = exp(-1/(Fs*tau)); tau 0 = exact jump.
struct Smoother {
    double y = 0, a = 0;
    void setTime(double fs, double tauSeconds) noexcept { a = tauSeconds > 0 ? std::exp(-1.0 / (fs * tauSeconds)) : 0.0; }
    void snap(double v) noexcept { y = v; }
    double next(double target) noexcept {
        const double d = target - y;
        y = (std::fabs(d) <= 1e-15 * std::max(1.0, std::fabs(target))) ? target : y + (1.0 - a) * d;
        return y;
    }
    bool settled(double target) const noexcept { return y == target; }
};

// Linear ramp 0..1 (equal-sum crossfades). Advances by 1/len per sample toward target 0 or 1.
struct Ramp {
    double w = 0, inc = 1;
    void setLength(int samples) noexcept { inc = 1.0 / double(std::max(1, samples)); }
    double next(bool on) noexcept {
        w = on ? std::min(1.0, w + inc) : std::max(0.0, w - inc);
        return w;
    }
};

// Transposed direct form II biquad, a0-normalized.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void identity() noexcept { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    void reset() noexcept { z1 = z2 = 0; }
    // Second-order Butterworth low-pass (bilinear), fc clamped below 0.45 fs.
    void lowpass(double fs, double fc) noexcept {
        fc = std::clamp(fc, 1.0, 0.45 * fs);
        const double k = std::tan(kPi * fc / fs), r2 = std::numbers::sqrt2;
        const double n = 1.0 / (1.0 + r2 * k + k * k);
        b0 = k * k * n; b1 = 2 * b0; b2 = b0;
        a1 = 2.0 * (k * k - 1.0) * n; a2 = (1.0 - r2 * k + k * k) * n;
    }
    // Second-order Butterworth high-pass (bilinear) - same form as dsp::ButterworthHpf.
    void highpass(double fs, double fc) noexcept {
        fc = std::clamp(fc, 1e-3, 0.45 * fs);
        const double k = std::tan(kPi * (fc / fs)), r2 = std::sqrt(2.0);
        const double n = 1.0 / (1.0 + r2 * k + k * k);
        b0 = n; b1 = -2.0 * n; b2 = n;
        a1 = 2.0 * (k * k - 1.0) * n; a2 = (1.0 - r2 * k + k * k) * n;
    }
    // RBJ cookbook high shelf (S=1 slope form with Q=0.707); gainDb == 0 gives exact identity.
    void highShelf(double fs, double f0, double gainDb) noexcept {
        if (gainDb == 0.0) { identity(); return; }
        f0 = std::clamp(f0, 10.0, 0.45 * fs);
        const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * f0 / fs;
        const double cw = std::cos(w), alpha = std::sin(w) / (2.0 * 0.70710678118654752);
        const double sA = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1) - (A - 1) * cw + sA;
        b0 = A * ((A + 1) + (A - 1) * cw + sA) / a0;
        b1 = -2 * A * ((A - 1) + (A + 1) * cw) / a0;
        b2 = A * ((A + 1) + (A - 1) * cw - sA) / a0;
        a1 = 2 * ((A - 1) - (A + 1) * cw) / a0;
        a2 = ((A + 1) - (A - 1) * cw - sA) / a0;
    }
};

// Integer delay line with power-of-two ring. delay() in [0, capacity-1].
class DelayLine {
public:
    void allocate(int maxDelay) {
        int n = 1;
        while (n < maxDelay + 4) n <<= 1;
        buf_.assign(std::size_t(n), 0.0);
        mask_ = n - 1;
        pos_ = 0;
    }
    void setDelay(int d) noexcept { d_ = std::clamp(d, 0, mask_); }
    int delay() const noexcept { return d_; }
    double process(double x) noexcept {
        buf_[std::size_t(pos_)] = x;
        const double y = buf_[std::size_t((pos_ - d_) & mask_)];
        pos_ = (pos_ + 1) & mask_;
        return y;
    }
    // Read k samples back from the most recently written sample (k=0 newest).
    double tap(int k) const noexcept { return buf_[std::size_t((pos_ - 1 - k) & mask_)]; }
    void clear() noexcept { std::fill(buf_.begin(), buf_.end(), 0.0); pos_ = 0; }
    std::vector<double>& storage() noexcept { return buf_; }
    int& position() noexcept { return pos_; }
    int capacity() const noexcept { return mask_ + 1; }
private:
    std::vector<double> buf_{0.0};
    int mask_ = 0, pos_ = 0, d_ = 0;
};

// Deterministic xoshiro256** stream seeded through SplitMix64. Block-independent: one draw per
// consuming sample, never per callback.
struct Rng {
    uint64_t s[4] = {1, 2, 3, 4};
    static uint64_t splitmix(uint64_t& x) noexcept {
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Seed from instance seed + slot + component + stream identifiers.
    void seed(uint64_t instance, uint64_t slot, uint64_t component, uint64_t stream) noexcept {
        uint64_t x = instance ^ (slot * 0xD1B54A32D192ED03ull) ^ (component * 0xABC98388FB8FAC03ull) ^ (stream * 0x8CB92BA72F3D8DD7ull);
        for (auto& v : s) v = splitmix(x);
        if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;
    }
    static uint64_t rotl(uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    uint64_t next() noexcept {
        const uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45);
        return r;
    }
    double uniform() noexcept { return double(next() >> 11) * 0x1.0p-53; }           // [0,1)
    double gaussian() noexcept {                                                     // Box-Muller, one per call
        const double u1 = (double(next() >> 11) + 0.5) * 0x1.0p-53, u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * kPi * u2);
    }
};

// Wait-free single-producer/single-consumer triple buffer for POD snapshots.
template <class T>
class TripleBuffer {
public:
    // Producer: write into back(), then publish().
    T& back() noexcept { return buf_[back_]; }
    void publish() noexcept { back_ = mid_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kMask; }
    // Consumer: returns true when a newer snapshot was taken; front() then holds it.
    bool acquire() noexcept {
        if (!(mid_.load(std::memory_order_acquire) & kDirty)) return false;
        front_ = mid_.exchange(front_, std::memory_order_acq_rel) & kMask;
        return true;
    }
    const T& front() const noexcept { return buf_[front_]; }
private:
    static constexpr uint8_t kDirty = 4, kMask = 3;
    T buf_[3]{};
    uint8_t back_ = 0, front_ = 1;
    std::atomic<uint8_t> mid_{2};
};

}  // namespace cd::engine
