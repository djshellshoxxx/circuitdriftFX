#include "control/CreativeMotion.h"
#include <algorithm>
#include <cmath>

namespace cd::control {

// SplitMix64, Sebastiano Vigna, public domain (https://prng.di.unimi.it/splitmix64.c).
uint64_t splitmix64(uint64_t& x) noexcept {
    uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

uint64_t deriveSeed(uint64_t instanceSeed, uint32_t slot, const char* uuid, StreamType type) noexcept {
    uint64_t st = instanceSeed;
    uint64_t h = splitmix64(st);
    auto mix = [&](uint64_t v) { st = h ^ v; h = splitmix64(st); };
    mix(slot);
    for (const char* p = uuid; p && *p; ++p) mix(static_cast<unsigned char>(*p));
    mix(0x100000000ULL | static_cast<uint32_t>(type));
    return h;
}

static inline uint64_t rotl(uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

void Xoshiro256ss::seed(uint64_t s) noexcept {
    for (auto& w : s_) w = splitmix64(s);
    hasSpare_ = false;
}
// xoshiro256** 1.0, Blackman & Vigna, public domain (https://prng.di.unimi.it/xoshiro256starstar.c).
uint64_t Xoshiro256ss::next() noexcept {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9, t = s_[1] << 17;
    s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
    s_[2] ^= t; s_[3] = rotl(s_[3], 45);
    return result;
}
double Xoshiro256ss::uniform01Open() noexcept { return static_cast<double>((next() >> 11) + 1) * 0x1.0p-53; }
double Xoshiro256ss::gaussian() noexcept {
    if (hasSpare_) { hasSpare_ = false; return spare_; }
    const double r = std::sqrt(-2.0 * std::log(uniform01Open())), th = 2.0 * M_PI * uniform01Open();
    spare_ = r * std::sin(th); hasSpare_ = true;
    return r * std::cos(th);
}

bool CreativeMotion::prepare(double fs, uint64_t seed) noexcept {
    if (!std::isfinite(fs) || !(fs > 0)) return false;
    rng_.seed(seed);
    const double tau[3] = {0.2, 2.0, 20.0};
    for (int j = 0; j < 3; ++j) {
        a_[j] = std::exp(-0.01 / tau[j]);
        s_[j] = std::sqrt(1.0 - a_[j] * a_[j]);
        u_[j] = rng_.gaussian();   // stationary start (unit variance)
    }
    inc_ = 100.0 / fs; phase_ = 0;
    q_ = std::tanh((u_[0] + u_[1] + u_[2]) / std::sqrt(3.0));
    return true;
}
void CreativeMotion::tick() noexcept {
    for (int j = 0; j < 3; ++j) u_[j] = a_[j] * u_[j] + s_[j] * rng_.gaussian();
    q_ = std::tanh((u_[0] + u_[1] + u_[2]) / std::sqrt(3.0));
}
void CreativeMotion::advanceSample() noexcept {
    phase_ += inc_;
    while (phase_ >= 1.0) { phase_ -= 1.0; tick(); }
}

bool PitchDriftDelay::prepare(double fs) noexcept {
    if (!std::isfinite(fs) || !(fs > 0) || fs > 1e7) return false;
    std::size_t n = 8;
    const std::size_t need = static_cast<std::size_t>(std::ceil(0.008 * fs)) + 4;
    while (n < need) n <<= 1;
    ring_.assign(n, 0.0); mask_ = n - 1; fs_ = fs;
    reset();
    return true;
}
void PitchDriftDelay::reset() noexcept {
    std::fill(ring_.begin(), ring_.end(), 0.0);
    pos_ = 0; cur_ = kBaseMs * 1e-3 * fs_;
}

void PitchDriftDelay::getState(double* out) const noexcept {
    if (!out || ring_.empty()) return;
    out[0] = static_cast<double>(pos_ & mask_);
    out[1] = cur_;
    std::copy(ring_.begin(), ring_.end(), out + 2);
}

bool PitchDriftDelay::validState(const double* in) const noexcept {
    if (!in || ring_.empty()) return false;
    const double n = static_cast<double>(ring_.size());
    if (!std::isfinite(in[0]) || in[0] < 0.0 || in[0] >= n || in[0] != std::floor(in[0])) return false;
    // process() reads taps up to floor(cur)+2 behind the write position: keep cur within the ring.
    if (!std::isfinite(in[1]) || in[1] < 1.0 || in[1] > n - 3.0) return false;
    for (std::size_t i = 0; i < ring_.size(); ++i)
        if (!std::isfinite(in[2 + i])) return false;
    return true;
}

bool PitchDriftDelay::setState(const double* in) noexcept {
    if (!validState(in)) return false;
    pos_ = static_cast<std::size_t>(in[0]);
    cur_ = in[1];
    std::copy(in + 2, in + 2 + ring_.size(), ring_.begin());
    return true;
}

double PitchDriftDelay::process(double x, double d, double q) noexcept {
    if (!enabled_ || ring_.empty()) return x;
    if (!std::isfinite(x)) x = 0.0;
    d = std::isfinite(d) ? std::clamp(d, 0.0, 1.0) : 0.0;
    q = std::isfinite(q) ? std::clamp(q, -1.0, 1.0) : 0.0;
    ring_[pos_ & mask_] = x;
    const double minD = 1.0;  // 4-point interpolation needs one future-side tap
    const double target = std::max(minD, (kBaseMs + kDevMs * d * q) * 1e-3 * fs_);
    const double step = kSlew;  // seconds/second == samples/sample
    cur_ += std::clamp(target - cur_, -step, step);
    const double i0f = std::floor(cur_), f = cur_ - i0f;
    const std::size_t i0 = static_cast<std::size_t>(i0f);
    auto tap = [&](std::size_t off) { return ring_[(pos_ - off) & mask_]; };
    const double pm1 = tap(i0 - 1), p0 = tap(i0), p1 = tap(i0 + 1), p2 = tap(i0 + 2);
    // 4-point Lagrange on nodes -1,0,1,2 at position f
    const double cm1 = -f * (f - 1) * (f - 2) / 6.0, c0 = (f + 1) * (f - 1) * (f - 2) / 2.0;
    const double c1 = -(f + 1) * f * (f - 2) / 2.0, c2 = (f + 1) * f * (f - 1) / 6.0;
    ++pos_;
    return cm1 * pm1 + c0 * p0 + c1 * p1 + c2 * p2;
}

}  // namespace cd::control
