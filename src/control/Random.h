// cd_control: deterministic PRNG (xoshiro256**) + Gaussian (Box-Muller with cached spare).
// Threading: plain value types, no allocation, noexcept; one owner thread per instance.
// State is a POD so it can be checkpointed byte-for-byte.
#pragma once
#include <cmath>
#include <cstdint>
#include <numbers>

namespace cd::control {

inline uint64_t splitmix64(uint64_t& s) noexcept {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Seed derivation from instance seed, slot and stream (component UUID hash may be folded into `stream`).
inline uint64_t deriveSeed(uint64_t instanceSeed, uint32_t slot, uint32_t stream) noexcept {
    uint64_t s = instanceSeed ^ (0xD1B54A32D192ED03ull * (uint64_t(slot) + 1));
    (void)splitmix64(s);
    s ^= 0x8CB92BA72F3D8DD7ull * (uint64_t(stream) + 1);
    return splitmix64(s);
}

struct RngState {
    uint64_t s[4]{};
    double spare = 0.0;
    uint32_t hasSpare = 0;
};

class GaussianRng {
public:
    void seed(uint64_t seed) noexcept {
        for (auto& w : st_.s) w = splitmix64(seed);
        st_.hasSpare = 0;
        st_.spare = 0.0;
    }
    uint64_t nextU64() noexcept {
        auto* s = st_.s;
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
    // Uniform in the open interval (0,1).
    double uniform() noexcept { return (double(nextU64() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    double gaussian() noexcept {
        if (st_.hasSpare) { st_.hasSpare = 0; return st_.spare; }
        const double u1 = uniform(), u2 = uniform();
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double th = 2.0 * std::numbers::pi * u2;
        st_.spare = r * std::sin(th);
        st_.hasSpare = 1;
        return r * std::cos(th);
    }
    const RngState& state() const noexcept { return st_; }
    void setState(const RngState& s) noexcept { st_ = s; }

private:
    static uint64_t rotl(uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    RngState st_{};
};

}  // namespace cd::control
