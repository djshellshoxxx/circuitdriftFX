// CircuitDrift benchmark: per-callback Engine::process time at 48 kHz / 128-frame stereo on the
// FACTORY SYNTHETIC diode-clipper graph for Eco, Studio, Render and 1x Zero Latency.
// Not a ctest test (target cd_bench, built on request). Reports p50/p99/max callback time and the
// share of the 128-frame deadline (2.667 ms), then compares with the spec targets (proposed):
// Eco p99 <= 0.35 ms, Studio <= 1.0 ms, Render <= 4.0 ms, 1x Zero Latency < 0.040 ms.
// The numbers describe THE MACHINE THIS RAN ON ONLY: the CPU model is printed and the output states
// "NOT reference hardware"; no target is ever claimed met on reference hardware here, and the
// fixture is synthetic (no physical validation, G01 open).
// Usage: cd_bench [callbacks=20000] [warmup=2000]. Build Release without sanitizers.
//        cd_bench --golden <out.bin>            writes the deterministic output of every case (numerical-
//                                               equivalence capture for performance work; prints FNV-1a hashes)
//        cd_bench --compare <ref.bin>           re-runs the golden cases and prints bit-exactness / max abs diff
//                                               relative to the signal peak against a previously captured file
//        cd_bench --count [case 0..3]           short deterministic run (for callgrind instruction counts)
// Owner: rtqual agent (benchmark only; never linked into product code).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "runtime/Engine.h"

namespace {
using namespace cd::runtime;
using Clock = std::chrono::steady_clock;

constexpr int kFrames = 128;
constexpr double kFs = 48000.0;
constexpr double kDeadlineMs = 1000.0 * kFrames / kFs;

std::string cpuModel() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            const auto p = line.find(':');
            if (p != std::string::npos) return line.substr(p + 2);
        }
    }
    return "unknown CPU (no /proc/cpuinfo model name)";
}

struct Case { const char* name; QualityMode mode; bool zeroLatency; double targetMs; const char* cmp; };

struct Result { double p50, p99, max; int64_t failed; };

Result run(const Case& c, int callbacks, int warmup) {
    EngineConfig cfg;
    cfg.sampleRate = kFs; cfg.maxBlock = kFrames; cfg.channels = 2;
    cfg.quality.mode = c.zeroLatency ? QualityMode::Manual : c.mode;
    cfg.quality.manualFactor = 1;
    cfg.quality.zeroLatency = c.zeroLatency;
    cfg.circuit = syntheticDiodeClipper();
    cfg.initial.autoGain = true;
    Engine eng;
    std::string err;
    if (!eng.prepare(cfg, err)) { std::printf("prepare failed (%s): %s\n", c.name, err.c_str()); return {0, 0, 0, -1}; }
    std::vector<double> in[2], out[2];
    for (int ch = 0; ch < 2; ++ch) { in[ch].resize(kFrames); out[ch].resize(kFrames); }
    std::vector<double> times;
    times.reserve(std::size_t(callbacks));
    int64_t pos = 0;
    for (int k = 0; k < warmup + callbacks; ++k) {
        for (int i = 0; i < kFrames; ++i) {   // musical-level stereo test tone (synthetic)
            const double t = double(pos + i) / kFs;
            in[0][std::size_t(i)] = 0.4 * std::sin(2 * M_PI * 110 * t) + 0.1 * std::sin(2 * M_PI * 1870 * t);
            in[1][std::size_t(i)] = 0.3 * std::sin(2 * M_PI * 173 * t);
        }
        const double* ip[2] = {in[0].data(), in[1].data()};
        double* op[2] = {out[0].data(), out[1].data()};
        const auto t0 = Clock::now();
        eng.process(ProcessContext{kFs, pos, 0, kFrames, false}, AudioBuses{2, ip, op}, {});
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (k >= warmup) times.push_back(ms);
        pos += kFrames;
    }
    std::sort(times.begin(), times.end());
    const auto q = [&](double p) { return times[std::min(times.size() - 1, std::size_t(p * double(times.size())))]; };
    return {q(0.50), q(0.99), times.back(), eng.diagnostics().circuit.failedSubsteps};
}

// Deterministic golden run: fixed tone, fixed blocks, normal and hot (diode-driving) level per case.
std::vector<double> goldenRun(const Case& c, double level, int blocks) {
    EngineConfig cfg;
    cfg.sampleRate = kFs; cfg.maxBlock = kFrames; cfg.channels = 2;
    cfg.quality.mode = c.zeroLatency ? QualityMode::Manual : c.mode;
    cfg.quality.manualFactor = 1;
    cfg.quality.zeroLatency = c.zeroLatency;
    cfg.circuit = syntheticDiodeClipper();
    cfg.initial.autoGain = true;
    Engine eng;
    std::string err;
    std::vector<double> all;
    if (!eng.prepare(cfg, err)) return all;
    std::vector<double> in[2], out[2];
    for (int ch = 0; ch < 2; ++ch) { in[ch].resize(kFrames); out[ch].resize(kFrames); }
    int64_t pos = 0;
    for (int k = 0; k < blocks; ++k) {
        for (int i = 0; i < kFrames; ++i) {
            const double t = double(pos + i) / kFs;
            in[0][std::size_t(i)] = level * (0.4 * std::sin(2 * M_PI * 110 * t) + 0.1 * std::sin(2 * M_PI * 1870 * t));
            in[1][std::size_t(i)] = level * 0.3 * std::sin(2 * M_PI * 173 * t);
        }
        const double* ip[2] = {in[0].data(), in[1].data()};
        double* op[2] = {out[0].data(), out[1].data()};
        eng.process(ProcessContext{kFs, pos, 0, kFrames, false}, AudioBuses{2, ip, op}, {});
        for (int ch = 0; ch < 2; ++ch) all.insert(all.end(), out[ch].begin(), out[ch].end());
        pos += kFrames;
    }
    return all;
}

std::uint64_t fnv(const std::vector<double>& v) {
    std::uint64_t h = 1469598103934665603ull;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(v.data());
    for (std::size_t i = 0; i < v.size() * sizeof(double); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

const Case kCases[] = {
    {"Eco", QualityMode::Eco, false, 0.35, "p99 <= 0.35 ms"},
    {"Studio", QualityMode::Studio, false, 1.0, "p99 <= 1.0 ms"},
    {"Render", QualityMode::Render, false, 4.0, "p99 <= 4.0 ms"},
    {"1x Zero-Latency", QualityMode::Manual, true, 0.040, "p99 < 0.040 ms"},
};

std::vector<double> goldenAll(int blocks, bool print) {
    std::vector<double> all;
    for (const Case& c : kCases)
        for (double level : {1.0, 4.0}) {
            const auto v = goldenRun(c, level, blocks);
            if (print) std::printf("golden %-16s level %.1f  n=%zu  fnv1a=%016llx\n", c.name, level, v.size(), (unsigned long long)fnv(v));
            all.insert(all.end(), v.begin(), v.end());
        }
    return all;
}

int goldenMain(const char* mode, const char* path) {
    const int blocks = 400;
    const auto all = goldenAll(blocks, true);
    if (std::string(mode) == "--golden") {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(all.data()), std::streamsize(all.size() * sizeof(double)));
        std::printf("wrote %zu doubles to %s\n", all.size(), path);
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<double> ref(all.size());
    f.read(reinterpret_cast<char*>(ref.data()), std::streamsize(ref.size() * sizeof(double)));
    if (!f || std::size_t(f.gcount()) != ref.size() * sizeof(double)) { std::printf("reference file size mismatch\n"); return 2; }
    double peak = 0, maxd = 0;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        peak = std::max(peak, std::fabs(ref[i]));
        const double d = std::fabs(all[i] - ref[i]);
        if (d != 0) ++differing;
        maxd = std::max(maxd, d);
    }
    std::printf("compare: %zu samples, %zu differ, max abs diff %.3e, signal peak %.3f, relative %.3e => %s\n", all.size(), differing,
                maxd, peak, peak > 0 ? maxd / peak : 0.0, differing == 0 ? "BIT-EXACT" : (maxd <= 1e-12 * peak ? "within 1e-12 of peak" : "EXCEEDS 1e-12"));
    return maxd <= 1e-12 * peak ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && (std::string(argv[1]) == "--golden" || std::string(argv[1]) == "--compare")) return goldenMain(argv[1], argv[2]);
    if (argc >= 2 && std::string(argv[1]) == "--count") {
        // Optional second argument selects one case (0 Eco, 1 Studio, 2 Render, 3 Zero-Latency).
        const int only = argc >= 3 ? std::atoi(argv[2]) : -1;
        for (int i = 0; i < 4; ++i)
            if (only < 0 || only == i) run(kCases[i], 300, 20);
        return 0;
    }
    const int callbacks = argc > 1 ? std::max(100, std::atoi(argv[1])) : 20000;
    const int warmup = argc > 2 ? std::max(0, std::atoi(argv[2])) : 2000;
    const std::string cpu = cpuModel();
    std::printf("MEASURED ON %s -- NOT reference hardware\n", cpu.c_str());
#if defined(__SANITIZE_ADDRESS__)
    std::printf("WARNING: sanitizer build; numbers are meaningless for targets\n");
#endif
#ifdef NDEBUG
    std::printf("build: NDEBUG defined (Release)\n");
#else
    std::printf("WARNING: assertions enabled / non-Release build\n");
#endif
    std::printf("48 kHz, %d-frame stereo callbacks (deadline %.3f ms), %d warm-up + %d measured, factory SYNTHETIC diode clipper\n",
                kFrames, kDeadlineMs, warmup, callbacks);
    std::printf("%-16s %9s %9s %9s %10s %10s  %s\n", "mode", "p50 ms", "p99 ms", "max ms", "p99 %dl", "max %dl", "spec target vs p99 (this machine only)");
    for (const Case& c : kCases) {
        const Result r = run(c, callbacks, warmup);
        if (r.failed < 0) return 1;
        const bool within = c.zeroLatency ? r.p99 < c.targetMs : r.p99 <= c.targetMs;
        std::printf("%-16s %9.4f %9.4f %9.4f %9.1f%% %9.1f%%  %s: %s on this machine (NOT reference hardware; failed substeps %lld)\n", c.name,
                    r.p50, r.p99, r.max, 100 * r.p99 / kDeadlineMs, 100 * r.max / kDeadlineMs, c.cmp, within ? "within" : "EXCEEDS",
                    (long long)r.failed);
    }
    std::printf("Targets are proposed spec numbers for reference hardware, which this is not; nothing here is a qualification.\n");
    return 0;
}
