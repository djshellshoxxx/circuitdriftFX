// cd_engine stress and benchmark (P03, T20/T25 parts). Deterministic stimuli and automation.
//  * healthy stress: loud 0 dBFS bass/transient stimulus at the maximum 36 dB drive (triode slot A,
//    NPN slot B), every quality mode -> finite,
//    bounded, zero solver failures and no fallback;
//  * hostile stress: per-block automation of every slot/global control, enum switches, quality
//    changes, extreme condition overlays and modulation endpoints -> every sample finite and within
//    the containment bound (fallbacks are counted and cleared with Reset, as the contract requires);
//  * rate smoke: 44.1/96/192 kHz (192 kHz Render = 16x) finite;
//  * benchmark (NOT gated): ns per stereo sample, both slots, 48 kHz, 128-frame blocks, per mode.
// Durations: Release 60 s Eco/Studio and 15 s Render of audio per stress case (Render costs 4x);
// sanitizer builds scale every duration by 1/24 (printed); CD_STRESS_SCALE overrides the scale.
// Synthetic fixture circuits only (G01 open).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <vector>
#include "core/ParameterRegistry.h"
#include "engine/CircuitEngine.h"

#if defined(__SANITIZE_ADDRESS__)
#define CD_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CD_SANITIZED 1
#endif
#endif
#ifndef CD_SANITIZED
#define CD_SANITIZED 0
#endif

namespace {
using namespace cd;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s | ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

double scale() {
    if (const char* s = std::getenv("CD_STRESS_SCALE")) { const double v = std::atof(s); if (v > 0) return v; }
    return CD_SANITIZED ? 1.0 / 24.0 : 1.0;
}

struct Lcg {
    uint64_t s;
    double next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return double(s >> 11) * 0x1.0p-53; }
};

// Loud deterministic program material: 50 Hz kick bursts, bass, chords, noise bursts, DC, gaps.
void stimulus(int64_t start, int n, double fs, double* l, double* r, Lcg& g) {
    for (int i = 0; i < n; ++i) {
        const double t = double(start + i) / fs;
        const double beat = std::fmod(t, 0.5);
        const double kick = 0.95 * std::exp(-beat * 18.0) * std::sin(2 * M_PI * (50.0 + 80.0 * std::exp(-beat * 30.0)) * beat);
        const double bass = 0.4 * std::sin(2 * M_PI * 55.0 * t) * (std::fmod(t, 4.0) < 2.0 ? 1.0 : 0.3);
        const double chord = 0.25 * (std::sin(2 * M_PI * 220 * t) + std::sin(2 * M_PI * 277.2 * t) + std::sin(2 * M_PI * 329.6 * t));
        const double noise = std::fmod(t, 3.0) > 2.7 ? 0.8 * (2 * g.next() - 1) : 0.0;
        const double gap = std::fmod(t, 7.0) > 6.5 ? 0.0 : 1.0;
        const double dc = std::fmod(t, 11.0) > 10.0 ? 0.3 : 0.0;
        l[i] = gap * (kick + bass + chord + noise) + dc;
        r[i] = gap * (kick + 0.8 * bass - chord + noise * 0.5) - dc;
    }
}

struct StressResult { double seconds = 0, peak = 0; long nonFinite = 0, outOfBound = 0, fallbacks = 0, failures = 0; bool ok = true; };

StressResult runStress(int mode, double seconds, bool hostile, double fs = 48000.0) {
    StressResult res;
    engine::CircuitEngine e;
    auto p = core::defaultValues();
    auto set = [&](const char* id, double v) { p[core::indexOf(id)] = v; };
    set("quality.mode", mode);
    set("slot.a.stage.drive", 36); set("slot.b.stage.drive", 36); set("input.gain", hostile ? 12 : 0);
    set("slot.b.stage.activeElement", 2);
    if (hostile) { set("pitchDrift.enabled", 1); set("analogNoise.enabled", 1); }
    engine::EngineConfig cfg;
    cfg.sampleRate = fs;
    cfg.channels = 2;
    cfg.maxBlock = 512;
    std::string err;
    if (!e.prepare(cfg, p, err)) { std::printf("stress prepare failed: %s\n", err.c_str()); res.ok = false; return res; }
    Lcg g{0xC1C1u + uint64_t(mode) * 77 + (hostile ? 5 : 0)};
    const int B = 512;
    std::vector<double> l(B), r(B), ol(B), orr(B), ext(B), pres(B);
    core::ModulationBlock mod{};
    engine::ProcessContext ctx{};
    ctx.sampleRate = fs;
    const int64_t total = int64_t(seconds * fs);
    for (int64_t pos = 0, blk = 0; pos < total; pos += B, ++blk) {
        stimulus(pos, B, fs, l.data(), r.data(), g);
        if (hostile) {
            auto rnd = [&](double lo, double hi) { return lo + (hi - lo) * g.next(); };
            const char* cont[][3] = {{"slot.a.stage.drive", "0", "36"}, {"slot.b.stage.drive", "0", "36"}, {"slot.a.stage.bias", "-1", "1"},
                                     {"slot.b.stage.bias", "-1", "1"}, {"ab.crossfade", "0", "1"}, {"mix", "0", "1"}, {"input.gain", "-12", "18"},
                                     {"output.gain", "-24", "12"}, {"slot.a.supply.mainsVoltage", "90", "130"},
                                     {"slot.b.supply.railCapacity", "10", "1000"}, {"slot.a.supply.railCapacity", "10", "1000"},
                                     {"slot.a.thermal.ambient", "10", "50"}, {"slot.a.thermal.mass", "0", "1"}, {"slot.b.thermal.driftCoefficient", "0", "2"},
                                     {"slot.a.condition.ageYears", "0", "60"}, {"slot.b.condition.tolerance", "0", "25"}, {"slot.a.condition.capLeakage", "0", "1"},
                                     {"slot.a.noise.humLevel", "-120", "-40"}, {"slot.b.noise.thermalNoise", "-120", "-50"}, {"slot.a.noise.microphonics", "0", "1"},
                                     {"slot.b.filter.lpfFrequency", "2000", "20000"}, {"hpf.frequency", "10", "250"}, {"slot.a.supply.sagSensitivity", "0", "1"}};
            for (const auto& c : cont) if (g.next() < 0.3) set(c[0], rnd(std::atof(c[1]), std::atof(c[2])));
            const char* enums[][2] = {{"slot.a.stage.activeElement", "3"}, {"slot.b.stage.activeElement", "3"}, {"slot.a.supply.rectifier", "2"},
                                      {"slot.b.supply.rectifier", "2"}, {"slot.a.transformer.core", "2"}, {"slot.b.condition.resistorType", "2"},
                                      {"slot.a.noise.humFrequency", "1"}, {"output.softClip", "1"}, {"hpf.enabled", "1"}, {"gainMatch.enabled", "1"},
                                      {"slot.a.filter.lpfEnabled", "1"}, {"quality.oversampling", "5"}};
            for (const auto& c : enums) if (g.next() < 0.02) set(c[0], std::floor(g.next() * (std::atof(c[1]) + 1)));
            for (auto& sm : mod.slot) {
                sm.ageYearsMin = rnd(0, 60); sm.toleranceSigmaAdd = rnd(0, 0.08); sm.leakageAdd = rnd(0, 0.1); sm.hfShelfDb = rnd(-6, 0);
                sm.ageNoiseDbfs = rnd(-90, -66); sm.reservoirCapDivisor = rnd(1, 4); sm.sagMacro = rnd(0, 1); sm.driveOffsetDb = rnd(0, 18);
                sm.biasOffset = rnd(0, 0.08); sm.fluxDriveFactor = rnd(1, 1.5); sm.thermalSensitivityScale = rnd(1, 5);
                sm.creativeCutoffOct = rnd(-0.15, 0.15); sm.creativeDelayMs = rnd(-1.5, 1.5); sm.creativeDelayActive = g.next() < 0.7;
                sm.recoveryTauS = g.next() < 0.5 ? rnd(0.05, 2) : 0; sm.sidechainMode = int(g.next() * 3);
            }
            for (int i = 0; i < B; ++i) { ext[std::size_t(i)] = 0.05 * g.next(); pres[std::size_t(i)] = g.next() < 0.1 ? 1.0 : 0.0; }
            mod.externalLoadA = ext.data();
            mod.transientPreserve = pres.data();
            if (g.next() < 0.01) {
                engine::ConditionSet cs{};
                for (auto& c : cs.c) {
                    c.valueScale = std::exp(rnd(std::log(0.05), std::log(20.0))); c.leakage = rnd(0, 1); c.esrScale = rnd(1, 1000);
                    c.noiseScale = rnd(0, 100); c.open = g.next() < 0.1; c.shortish = g.next() < 0.1;
                }
                e.submitConditions(int(g.next() * 2), cs);
            }
            if (g.next() < 0.005) ctx.epoch++;
        }
        const double* ip[2] = {l.data(), r.data()};
        double* op[2] = {ol.data(), orr.data()};
        e.process(ctx, ip, op, 2, nullptr, 0, B, p, mod);
        for (int i = 0; i < B; ++i)
            for (double v : {ol[std::size_t(i)], orr[std::size_t(i)]}) {
                if (!std::isfinite(v)) ++res.nonFinite;
                else if (std::fabs(v) > 8.0) ++res.outOfBound;
                else res.peak = std::max(res.peak, std::fabs(v));
            }
        if (e.solverFallbackActive()) { ++res.fallbacks; e.reset(engine::ResetKind::Warm); }
        ctx.sampleStart += B;
    }
    engine::TelemetryFrame t{};
    e.readTelemetry(t);
    res.failures = t.solverFailures;
    res.seconds = double(total) / fs;
    return res;
}
}  // namespace

int main() {
    const double sc = scale();
    std::printf("stress scale %.3f (%s build)\n", sc, CD_SANITIZED ? "sanitized" : "release");
    // Stress cases in parallel test threads (independent engines).
    struct Case { const char* name; int mode; double seconds; bool hostile; };
    const Case cases[] = {{"Eco healthy", 0, 60, false}, {"Studio healthy", 1, 60, false}, {"Render healthy", 2, 15, false},
                          {"Eco hostile", 0, 60, true}, {"Studio hostile", 1, 60, true}, {"Render hostile", 2, 15, true}};
    std::vector<std::future<StressResult>> futs;
    const auto t0 = std::chrono::steady_clock::now();
    for (const Case& c : cases) futs.push_back(std::async(std::launch::async, runStress, c.mode, c.seconds * sc, c.hostile, 48000.0));
    for (std::size_t k = 0; k < futs.size(); ++k) {
        const StressResult r = futs[k].get();
        const Case& c = cases[k];
        std::printf("stress %-15s %6.2f s audio: peak %.3f, non-finite %ld, |y|>8 %ld, fallbacks %ld, solver failures %ld\n", c.name, r.seconds,
                    r.peak, r.nonFinite, r.outOfBound, r.fallbacks, r.failures);
        CHECK(r.ok && r.nonFinite == 0 && r.outOfBound == 0, "%s finite and bounded", c.name);
        if (!c.hostile) CHECK(r.fallbacks == 0 && r.failures == 0, "%s healthy: no solver failure (%ld) or fallback (%ld)", c.name, r.failures, r.fallbacks);
    }
    std::printf("stress wall time %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    // Other sample rates (192 kHz Render = 16x at 3.072 MHz substeps).
    for (const double fs : {44100.0, 96000.0, 192000.0}) {
        const StressResult r = runStress(fs == 192000.0 ? 2 : 1, 0.25 * std::max(sc, 0.25), false, fs);
        CHECK(r.ok && r.nonFinite == 0 && r.outOfBound == 0 && r.failures == 0, "fs %g finite/bounded/no failures (%ld)", fs, r.failures);
        std::printf("rate smoke %6.0f Hz %s: peak %.3f failures %ld\n", fs, fs == 192000.0 ? "Render 16x" : "Studio 4x", r.peak, r.failures);
    }
    // Benchmark (not gated): stereo, both slots, 48 kHz, 128-frame blocks.
    const double benchSeconds = CD_SANITIZED ? 0.2 : 2.0;
    for (int mode = 0; mode < 3; ++mode) {
        engine::CircuitEngine e;
        auto p = core::defaultValues();
        p[core::indexOf("quality.mode")] = mode;
        engine::EngineConfig cfg;
        cfg.channels = 2;
        std::string err;
        if (!e.prepare(cfg, p, err)) { CHECK(false, "bench prepare %s", err.c_str()); continue; }
        const int B = 128;
        std::vector<double> l(B), r(B), ol(B), orr(B);
        core::ModulationBlock mod{};
        engine::ProcessContext ctx{};
        Lcg g{42};
        const int64_t total = int64_t(benchSeconds * 48000);
        double worstBlock = 0;
        const auto b0 = std::chrono::steady_clock::now();
        for (int64_t pos = 0; pos < total; pos += B) {
            stimulus(pos, B, 48000, l.data(), r.data(), g);
            for (int i = 0; i < B; ++i) { l[std::size_t(i)] *= 0.5; r[std::size_t(i)] *= 0.5; }
            const double* ip[2] = {l.data(), r.data()};
            double* op[2] = {ol.data(), orr.data()};
            const auto c0 = std::chrono::steady_clock::now();
            e.process(ctx, ip, op, 2, nullptr, 0, B, p, mod);
            worstBlock = std::max(worstBlock, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count());
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - b0).count() / double(total);
        static const char* names[3] = {"Eco (4x, cap 6)", "Studio (4x, cap 8)", "Render (16x, cap 12)"};
        std::printf("BENCH %-22s %8.0f ns/stereo sample  %6.3f ms per 128-frame block (max %.3f)  %5.1f%% of real time%s\n", names[mode], ns,
                    ns * 128 * 1e-6, worstBlock, ns * 48000 * 1e-7, CD_SANITIZED ? "  [sanitized build: not representative]" : "");
    }
    std::printf(g_fail ? "ENGINE STRESS FAILED (%d)\n" : "ENGINE STRESS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
