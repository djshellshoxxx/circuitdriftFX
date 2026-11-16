// CircuitDrift benchmark: per-callback Engine::process time at 48 kHz / 128-frame stereo on the
// FACTORY SYNTHETIC diode-clipper graph for Eco, Studio, Render and 1x Zero Latency.
// Not a ctest test (target cd_bench, built on request). Reports p50/p99/max callback time and the
// share of the 128-frame deadline (2.667 ms), then compares with the spec targets (proposed):
// Eco p99 <= 0.35 ms, Studio <= 1.0 ms, Render <= 4.0 ms, 1x Zero Latency < 0.040 ms.
// The numbers describe THE MACHINE THIS RAN ON ONLY: the CPU model is printed and the output states
// "NOT reference hardware"; no target is ever claimed met on reference hardware here, and the
// fixture is synthetic (no physical validation, G01 open).
// Usage: cd_bench [callbacks=20000] [warmup=2000]. Build Release without sanitizers.
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

}  // namespace

int main(int argc, char** argv) {
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
    const Case cases[] = {
        {"Eco", QualityMode::Eco, false, 0.35, "p99 <= 0.35 ms"},
        {"Studio", QualityMode::Studio, false, 1.0, "p99 <= 1.0 ms"},
        {"Render", QualityMode::Render, false, 4.0, "p99 <= 4.0 ms"},
        {"1x Zero-Latency", QualityMode::Manual, true, 0.040, "p99 < 0.040 ms"},
    };
    std::printf("%-16s %9s %9s %9s %10s %10s  %s\n", "mode", "p50 ms", "p99 ms", "max ms", "p99 %dl", "max %dl", "spec target vs p99 (this machine only)");
    for (const Case& c : cases) {
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
