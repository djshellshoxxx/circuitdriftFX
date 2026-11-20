// Plain-assert harness shared by the cd_control suites (same style as tests/test_main.cpp).
// All fixtures here are SYNTHETIC (G01 physical validation remains open).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

inline int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s | ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace th {
struct Lcg {
    uint64_t s = 12345;
    double next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return double(s >> 11) / 9007199254740992.0; }  // [0,1)
    double bipolar() { return 2.0 * next() - 1.0; }
};
inline std::vector<double> sine(double f, double amp, double fs, std::size_t n, double phase = 0) {
    std::vector<double> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = amp * std::sin(2.0 * std::numbers::pi * f * double(i) / fs + phase);
    return v;
}
inline double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }
}  // namespace th
