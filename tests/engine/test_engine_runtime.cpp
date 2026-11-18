// cd_engine runtime acceptance (P03): T20 thermal clock, block-partition invariance, declared
// latency, mix/trim exactness, A/B unity, T22 integrated HPF, T23 gains/RMS match/soft clip,
// containment and solver fallback, quality rates/transitions/staging, macro endpoints,
// checkpoint continuation, condition overlays and telemetry. Deterministic; every circuit is a
// SYNTHETIC FIXTURE (G01 open) - these tests prove software behaviour, not physical accuracy.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "core/ParameterRegistry.h"
#include "dsp/Oversampling.h"
#include "engine/CircuitEngine.h"
#include "engine/ThermalClock.h"

namespace {
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s | ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using namespace cd;
using engine::CircuitEngine;
using Buf = std::vector<std::vector<double>>;

void set(core::ParamValues& p, const char* id, double v) {
    const auto i = core::indexOf(id);
    if (i >= core::kParamCount) { std::printf("unknown parameter %s\n", id); ++g_fail; return; }
    p[i] = v;
}

struct Rig {
    CircuitEngine e;
    core::ParamValues p = core::defaultValues();
    engine::EngineConfig cfg{};
    core::ModulationBlock mod{};
    engine::ProcessContext ctx{};
    Rig(int channels = 1, double fs = 48000) { cfg.channels = channels; cfg.sampleRate = fs; cfg.maxBlock = 512; ctx.sampleRate = fs; }
    bool prepare() {
        std::string err;
        const bool ok = e.prepare(cfg, p, err);
        if (!ok) std::printf("prepare failed: %s\n", err.c_str());
        return ok;
    }
    // Processes `in` with a cyclic block pattern; optional zero-frame calls between blocks and
    // per-sample modulation arrays (offset by the block start).
    Buf run(const Buf& in, const std::vector<int>& blocks = {128}, bool zeroCalls = false,
            const std::vector<double>* ext = nullptr, const std::vector<double>* pres = nullptr) {
        const int ch = int(in.size()), n = int(in[0].size());
        Buf out(std::size_t(ch), std::vector<double>(std::size_t(n), 0.0));
        int pos = 0, bi = 0;
        while (pos < n) {
            const int b = std::min(blocks[std::size_t(bi++ % blocks.size())], n - pos);
            const double* ip[2] = {in[0].data() + pos, ch > 1 ? in[1].data() + pos : nullptr};
            double* op[2] = {out[0].data() + pos, ch > 1 ? out[1].data() + pos : nullptr};
            core::ModulationBlock m = mod;
            if (ext) m.externalLoadA = ext->data() + pos;
            if (pres) m.transientPreserve = pres->data() + pos;
            ctx.frames = uint32_t(b);
            e.process(ctx, ip, op, ch, nullptr, 0, b, p, m);
            if (zeroCalls) e.process(ctx, ip, op, ch, nullptr, 0, 0, p, m);
            ctx.sampleStart += b;
            pos += b;
        }
        return out;
    }
};

Buf sine(int ch, int n, double fs, double f, double a, double phase = 0) {
    Buf b(static_cast<std::size_t>(ch), std::vector<double>(static_cast<std::size_t>(n), 0.0));
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i) b[c][i] = a * std::sin(2 * M_PI * f * i / fs + phase + 0.3 * c);
    return b;
}
double quadAmp(const std::vector<double>& y, double fs, double f, int from, int to) {
    double sc = 0, cc = 0;
    for (int i = from; i < to; ++i) { sc += y[i] * std::sin(2 * M_PI * f * i / fs); cc += y[i] * std::cos(2 * M_PI * f * i / fs); }
    return 2 * std::hypot(sc, cc) / (to - from);
}
double rms(const std::vector<double>& y, int from, int to) {
    double s = 0;
    for (int i = from; i < to; ++i) s += y[i] * y[i];
    return std::sqrt(s / (to - from));
}
double maxDiff(const Buf& a, const Buf& b) {
    double w = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = 0; i < a[c].size(); ++i) w = std::max(w, std::fabs(a[c][i] - b[c][i]));
    return w;
}
bool allFinite(const Buf& a, double bound) {
    for (const auto& c : a) for (double v : c) if (!std::isfinite(v) || std::fabs(v) > bound) return false;
    return true;
}

// ---- T20 through the engine thermal clock ------------------------------------------------------
void t20EngineClock() {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        dsp::ThermalNetwork::Config c;
        c.n = 1; c.capacity[0] = 2.0; c.toAmbient[0] = 0.2;
        engine::ThermalClock clk;
        CHECK(clk.prepare(c, fs, 298.15), "clock prepare");
        const double p[1] = {1.0};
        long ticks = 0;
        const long n = long(std::llround(10.0 * fs));
        for (long i = 0; i < n; ++i) { clk.accumulate(p); ticks += clk.advance() ? 1 : 0; }
        const double exact = 298.15 + 5.0 * (1.0 - std::exp(-1.0));
        CHECK(ticks == 10000, "fs %g ticks %ld", fs, ticks);
        CHECK(std::fabs(clk.network().temperature(0) - exact) <= 0.002, "fs %g T20 err %g K", fs, clk.network().temperature(0) - exact);
    }
}

// ---- block partition invariance ------------------------------------------------------------------
void partitionInvariance() {
    const int n = 9000;
    Buf in(2, std::vector<double>(n));
    std::vector<double> ext(n), pres(n);
    for (int i = 0; i < n; ++i) {
        const double t = i / 48000.0;
        in[0][i] = 0.4 * std::sin(2 * M_PI * 110 * t) + 0.2 * std::sin(2 * M_PI * 1900 * t) * (i > 3000 && i < 3400 ? 3.0 : 1.0);
        in[1][i] = 0.3 * std::sin(2 * M_PI * 165 * t + 0.4) + ((i * 7919) % 101 - 50) * 0.002;
        ext[i] = 0.01 * (1 + std::sin(2 * M_PI * 3 * t));
        pres[i] = (i / 500) % 2 ? 0.8 : 0.0;
    }
    auto configure = [](Rig& r) {
        set(r.p, "slot.a.noise.thermalNoise", -80); set(r.p, "slot.b.noise.thermalNoise", -85);
        set(r.p, "slot.a.noise.humLevel", -70); set(r.p, "slot.a.noise.microphonics", 0.6);
        set(r.p, "slot.a.stage.drive", 18); set(r.p, "slot.a.stage.bias", 0.3);
        set(r.p, "slot.b.stage.activeElement", 2); set(r.p, "slot.b.supply.rectifier", 2);
        set(r.p, "slot.b.filter.lpfEnabled", 1); set(r.p, "slot.b.filter.lpfFrequency", 6000);
        set(r.p, "ab.crossfade", 0.3); set(r.p, "gainMatch.enabled", 1); set(r.p, "analogNoise.enabled", 1);
        set(r.p, "slot.a.thermal.mass", 0.0); set(r.p, "slot.a.condition.ageYears", 30);
        r.mod.slot[0].sidechainMode = 2; r.mod.slot[0].sagMacro = 0.7; r.mod.slot[0].ageNoiseDbfs = -80;
        r.mod.slot[1].hfShelfDb = -4;
    };
    Buf ref;
    engine::TelemetryFrame tRef{};
    for (const std::vector<int>& part : std::vector<std::vector<int>>{{512}, {1}, {17}, {64}, {127}, {8192}, {7, 300, 1, 64}}) {
        Rig r(2);
        configure(r);
        if (!r.prepare()) return;
        const Buf out = r.run(in, part, part.size() > 1 || part[0] == 17, &ext, &pres);
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        if (ref.empty()) { ref = out; tRef = t; CHECK(allFinite(out, 4.0), "partition reference finite"); continue; }
        const double d = maxDiff(ref, out);
        CHECK(d <= 1e-9, "partition %d (x%zu) max diff %g", part[0], part.size(), d);
        CHECK(t.slot[0].temperatureK[0][2] == tRef.slot[0].temperatureK[0][2] && t.slot[1].railVolts == tRef.slot[1].railVolts,
              "partition %d telemetry state differs", part[0]);
    }
    std::printf("partition invariance: 7 partitions incl. 0-frame calls compared\n");
}

// ---- declared latency ------------------------------------------------------------------------------
int expectedPad(dsp::OsFamily fam) {
    double mx = 0;
    for (int f : {1, 2, 4, 8, 16}) { dsp::Oversampler os; os.prepare({f, fam, 0.45, 100.0}, 64); mx = std::max(mx, os.latency()); }
    return int(std::ceil(mx - 1e-9));
}

void latency() {
    struct Case { const char* name; int mode, over, phase, zero, pitch; int expect; bool exactWet; };
    const int fir = expectedPad(dsp::OsFamily::Fir), iir = expectedPad(dsp::OsFamily::Iir), cre = int(std::lround(1.5e-3 * 48000));
    const Case cases[] = {{"Studio FIR", 1, 0, 0, 0, 0, 2 * fir, true}, {"Render FIR", 2, 0, 0, 0, 0, 2 * fir, true},
                          {"Draft 1x FIR", 1, 1, 0, 0, 0, 2 * fir, true}, {"Studio IIR", 1, 0, 1, 0, 0, 2 * iir, false},
                          {"ZeroLatency", 1, 0, 0, 1, 0, 0, true}, {"PitchDrift", 1, 0, 0, 0, 1, 2 * fir + cre, true}};
    for (const Case& c : cases) {
        for (int wet = 0; wet < 2; ++wet) {
            Rig r(1);
            set(r.p, "quality.mode", c.mode); set(r.p, "quality.oversampling", c.over); set(r.p, "quality.phase", c.phase);
            set(r.p, "quality.zeroLatency", c.zero); set(r.p, "pitchDrift.enabled", c.pitch);
            set(r.p, "hpf.enabled", 0); set(r.p, "output.softClip", wet ? 1 : 0); set(r.p, "mix", wet ? 1.0 : 0.0);
            set(r.p, "slot.a.stage.drive", 0); set(r.p, "slot.b.stage.drive", 0);
            if (!r.prepare()) return;
            CHECK(r.e.latencySamples() == c.expect, "%s latency %d expected %d", c.name, r.e.latencySamples(), c.expect);
            const int n = 2048, at = 100;
            Buf in(1, std::vector<double>(n, 0.0));
            in[0][at] = 0.05;
            const Buf out = r.run(in, {256});
            int arg = 0;
            for (int i = 0; i < n; ++i) if (std::fabs(out[0][i]) > std::fabs(out[0][arg])) arg = i;
            const int L = r.e.latencySamples();
            if (!wet) {
                double other = 0;
                for (int i = 0; i < n; ++i) if (i != at + L) other = std::max(other, std::fabs(out[0][i]));
                CHECK(arg == at + L && std::fabs(out[0][at + L] - 0.05) <= 1e-15 && other <= 1e-15, "%s dry impulse at %d (expect %d) other %g",
                      c.name, arg - at, L, other);
            } else {
                CHECK(c.exactWet ? arg == at + L : std::abs(arg - at - L) <= 2, "%s wet impulse peak at %d expect %d", c.name, arg - at, L);
            }
            engine::TelemetryFrame t{};
            CHECK(r.e.readTelemetry(t) && t.latencySamples == L, "%s telemetry latency", c.name);
        }
    }
    std::printf("latency: FIR pad %d, IIR pad %d, total FIR %d samples @48k\n", fir, iir, 2 * fir);
}

// ---- mix = 0: latency-aligned dry x trims; T23 gain ranges -------------------------------------------
void mixZeroAndGains() {
    const int n = 4000;
    const double pairs[][2] = {{-24, -24}, {-24, 24}, {0, -3}, {6, 0}, {24, -24}, {24, 0}};
    for (const auto& gp : pairs) {
        const double gi = gp[0], go = gp[1];
        {
            // Keep |output| below the emergency containment bound (8) so the trims are measured.
            const double sc = std::min(1.0, 2.0 / std::pow(10.0, (gi + go) / 20.0));
            Buf in(2, std::vector<double>(n));
            for (int i = 0; i < n; ++i) { in[0][i] = sc * (0.3 * std::sin(0.031 * i) + 0.1 * std::sin(0.77 * i)); in[1][i] = sc * 0.2 * std::cos(0.05 * i); }
            Rig r(2);
            set(r.p, "mix", 0); set(r.p, "hpf.enabled", 0); set(r.p, "output.softClip", 0);
            set(r.p, "input.gain", gi); set(r.p, "output.gain", go); set(r.p, "quality.oversampling", 1);
            if (!r.prepare()) return;
            const int L = r.e.latencySamples();
            const Buf out = r.run(in, {333});
            const double g = std::pow(10.0, (gi + go) / 20.0);
            double worst = 0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) worst = std::max(worst, std::fabs(out[c][i] - (i >= L ? in[c][i - L] * g : 0.0)));
            CHECK(worst <= 1e-12 * std::max(1.0, g), "mix0 in %g out %g dB: |y - delayed dry*trims| %g", gi, go, worst);
            const double measured = 20 * std::log10(rms(out[0], L + 100, n) / rms(in[0], 100, n - L));
            CHECK(std::fabs(measured - (gi + go)) <= 0.01, "T23 gain %g+%g measured %g dB", gi, go, measured);
        }
    }
}

// ---- identical A/B slots: unity < 0.01 dB ---------------------------------------------------------------
void abUnity() {
    const Buf in = sine(2, 6000, 48000, 330, 0.4);
    Buf outs[3];
    const double cf[3] = {0.0, 0.5, 1.0};
    for (int k = 0; k < 3; ++k) {
        Rig r(2);
        set(r.p, "ab.crossfade", cf[k]); set(r.p, "slot.a.stage.drive", 18); set(r.p, "slot.b.stage.drive", 18);
        if (!r.prepare()) return;
        outs[k] = r.run(in, {256});
    }
    for (int k = 1; k < 3; ++k) {
        CHECK(maxDiff(outs[0], outs[k]) <= 1e-12, "A/B crossfade %g identical slots diff %g", cf[k], maxDiff(outs[0], outs[k]));
        const double db = 20 * std::log10(rms(outs[k][0], 2000, 6000) / rms(outs[0][0], 2000, 6000));
        CHECK(std::fabs(db) < 0.01, "A/B unity %g dB", db);
    }
}

// ---- T22 integrated HPF ------------------------------------------------------------------------------------
void t22Integrated() {
    for (double fs : {48000.0, 96000.0}) {
        auto measure = [&](double f, bool wet, bool hpf) {
            Rig r(1, fs);
            set(r.p, "mix", wet ? 1.0 : 0.0); set(r.p, "quality.oversampling", 1); set(r.p, "hpf.enabled", hpf ? 1 : 0);
            set(r.p, "slot.a.stage.drive", 0); set(r.p, "slot.b.stage.drive", 0);
            if (!r.prepare()) return 0.0;
            const int n = int(1.0 * fs), from = int(0.6 * fs), to = int(1.0 * fs);  // 6 periods at 15 Hz after 0.6 s settle
            const Buf out = r.run(sine(1, n, fs, f, 0.1), {512});
            return quadAmp(out[0], fs, f, from, to);
        };
        const double dry15 = measure(15, false, true), dry1k = measure(1000, false, true);
        const double rel = 20 * std::log10(dry15 / dry1k);
        CHECK(std::fabs(rel + 3.0103) <= 0.05, "T22 fs %g dry-endpoint 15 Hz %g dB", fs, rel);
        if (fs == 48000.0) {
            const double wetOn = measure(15, true, true), wetOff = measure(15, true, false);
            const double ratio = 20 * std::log10(wetOn / wetOff);
            CHECK(std::fabs(ratio + 3.0103) <= 0.05, "T22 wet path HPF ratio at 15 Hz %g dB", ratio);
            std::printf("T22: fs %g dry 15 Hz %.4f dB, wet on/off ratio %.4f dB\n", fs, rel, ratio);
        }
    }
    // DC step: residual < -100 dBFS after 2 s (dry endpoint and full wet path).
    for (int wet = 0; wet < 2; ++wet) {
        Rig r(1);
        set(r.p, "mix", wet ? 1.0 : 0.0); set(r.p, "quality.oversampling", 1);
        if (!r.prepare()) return;
        const int n = 48000 * 11 / 5;  // residual window 2.0..2.2 s after the step
        Buf in(1, std::vector<double>(n, 0.5));
        const Buf out = r.run(in, {512});
        double worst = 0, mean = 0;
        for (int i = 96000; i < n; ++i) { worst = std::max(worst, std::fabs(out[0][i])); mean += out[0][i]; }
        mean /= (n - 96000);
        const double db = 20 * std::log10(std::fabs(mean) + 1e-300), pk = 20 * std::log10(worst + 1e-300);
        CHECK(db < -100.0 && (wet || pk < -100.0), "T22 DC residual %s mean %g dBFS peak %g dBFS", wet ? "wet" : "dry", db, pk);
        std::printf("T22 DC step %s: mean %.1f dBFS, peak %.1f dBFS (peak includes AC ripple/LF tails)\n", wet ? "wet" : "dry", db, pk);
    }
}

// ---- T23 RMS match and soft clip ----------------------------------------------------------------------------
void t23MatchAndClip() {
    {
        Rig r(1);
        set(r.p, "gainMatch.enabled", 1); set(r.p, "quality.oversampling", 1);
        set(r.p, "slot.a.stage.drive", 14); set(r.p, "slot.b.stage.drive", 14);  // wet ~ +7 dB: inside [0.25, 4]
        if (!r.prepare()) return;
        const int n = 48000 * 6;
        const Buf in = sine(1, n, 48000, 220, 0.25);
        const Buf out = r.run(in, {512});
        const double db = 20 * std::log10(rms(out[0], n - 48000, n) / rms(in[0], n - 48000 - r.e.latencySamples(), n - r.e.latencySamples()));
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        CHECK(std::fabs(db) <= 0.5 && t.gainMatch >= 0.25 && t.gainMatch <= 4.0, "T23 RMS match %g dB (gain %g)", db, t.gainMatch);
        std::printf("T23 RMS match: residual %.3f dB, compensation %.3f\n", db, t.gainMatch);
    }
    // Bound: the soft clip output is <= 1 at the oversampled rate; band-limiting decimation can
    // overshoot by at most the decimator chain's L1 norm (no true-peak claim, 01-runtime).
    double l1 = 0;
    {
        dsp::Oversampler os;
        os.prepare({4, dsp::OsFamily::Fir, 0.45, 100.0}, 64);
        std::vector<double> hi(64 * 4), lo(64);
        for (int ph = 0; ph < 4; ++ph) {
            os.reset();
            for (int blk = 0; blk < 8; ++blk) {
                std::fill(hi.begin(), hi.end(), 0.0);
                if (blk == 0) hi[std::size_t(ph)] = 1.0;
                os.down(hi.data(), lo.data(), 64);
                for (double v : lo) l1 += std::fabs(v);
            }
        }
    }
    struct Mode { const char* name; int over, zero; double bound; };
    for (const Mode m : {Mode{"Studio 4x", 0, 0, l1}, Mode{"Draft 1x", 1, 0, 1.0}, Mode{"ZeroLatency", 0, 1, 1.0}}) {
        Rig r(2);
        set(r.p, "quality.oversampling", m.over); set(r.p, "quality.zeroLatency", m.zero);
        set(r.p, "slot.a.stage.drive", 36); set(r.p, "output.gain", 12);
        if (!r.prepare()) return;
        const Buf out = r.run(sine(2, 9600, 48000, 997, 0.9), {256});
        double pk = 0;
        for (const auto& c : out) for (double v : c) pk = std::max(pk, std::fabs(v));
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        CHECK(pk <= m.bound && pk > 0.85 && t.clampedOutputs == 0 && !t.solverFallback, "soft clip %s peak %g (bound %g) clamps %ld", m.name, pk, m.bound,
              t.clampedOutputs);
        std::printf("soft clip %s: sample peak %.5f (bound %.5f)\n", m.name, pk, m.bound);
    }
}

// ---- containment and solver fallback ----------------------------------------------------------------------------
void containment() {
    const int n = 9600, L0 = 2400;
    // (a) injected non-finite wet samples
    {
        Rig r(1);
        if (!r.prepare()) return;
        const int L = r.e.latencySamples();
        const Buf in = sine(1, n, 48000, 440, 0.3);
        Buf first(1, std::vector<double>(in[0].begin(), in[0].begin() + L0));
        Buf second(1, std::vector<double>(in[0].begin() + L0, in[0].end()));
        r.run(first, {128});
        r.e.debugInjectWetNonFinite(8);
        const Buf out = r.run(second, {128});
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        CHECK(allFinite(out, 8.0), "non-finite containment output finite");
        CHECK(t.nonFiniteOutputs > 0 && t.solverFallback && t.needsReset, "non-finite flags %ld %d %d", t.nonFiniteOutputs, t.solverFallback, t.needsReset);
        double worst = 0;
        for (int i = 3000; i < n - L0; ++i) worst = std::max(worst, std::fabs(out[0][i] - in[0][L0 + i - L]));
        CHECK(worst <= 1e-12, "after fallback fade output is delayed dry (%g)", worst);
        r.e.reset(engine::ResetKind::Warm);
        CHECK(!r.e.solverFallbackActive(), "reset clears fallback");
        const Buf again = r.run(second, {128});
        CHECK(allFinite(again, 4.0), "processing after reset finite");
    }
    // (b) |y| > 8: clamp and fallback (soft clip disabled, +24 dB trim)
    {
        Rig r(1);
        set(r.p, "output.softClip", 0); set(r.p, "output.gain", 24); set(r.p, "slot.a.stage.drive", 24);
        if (!r.prepare()) return;
        const int L = r.e.latencySamples();
        const Buf in = sine(1, n, 48000, 440, 0.9);
        const Buf out = r.run(in, {128});
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        double pk = 0;
        for (double v : out[0]) pk = std::max(pk, std::fabs(v));
        CHECK(pk <= 8.0 && t.clampedOutputs > 0 && t.solverFallback, "clamp peak %g clamps %ld", pk, t.clampedOutputs);
        double worst = 0;
        for (int i = 4000; i < n; ++i) worst = std::max(worst, std::fabs(out[0][i] - in[0][i - L]));
        CHECK(worst <= 1e-12, "clamp fallback ends on delayed dry (%g)", worst);
    }
    // (c) forced solver failures: 3 consecutive -> 10 ms fade to delayed dry, needs-reset flag.
    {
        Rig r(1);
        if (!r.prepare()) return;
        const int L = r.e.latencySamples();
        const Buf in = sine(1, n, 48000, 440, 0.3);
        Buf first(1, std::vector<double>(in[0].begin(), in[0].begin() + L0));
        Buf second(1, std::vector<double>(in[0].begin() + L0, in[0].end()));
        r.run(first, {128});
        r.e.debugForceSolverFailure(true);
        const Buf out = r.run(second, {128});
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        CHECK(t.solverFailures >= 3 && t.solverFallback && t.needsReset, "solver fallback flags failures %ld", t.solverFailures);
        double worst = 0;
        for (int i = 1000; i < n - L0; ++i) worst = std::max(worst, std::fabs(out[0][i] - in[0][L0 + i - L]));
        CHECK(worst <= 1e-12 && allFinite(out, 4.0), "solver fallback converges to delayed dry (%g)", worst);
        r.e.debugForceSolverFailure(false);
        r.e.reset(engine::ResetKind::Cold);
        CHECK(!r.e.solverFallbackActive(), "cold reset clears solver fallback");
    }
    // (d) non-finite input is sanitized without fallback
    {
        Rig r(1);
        if (!r.prepare()) return;
        Buf in = sine(1, 2400, 48000, 440, 0.3);
        in[0][100] = NAN; in[0][101] = INFINITY;
        const Buf out = r.run(in, {128});
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        CHECK(allFinite(out, 4.0) && t.nonFiniteInputs == 2 && !t.solverFallback, "input sanitize %ld", t.nonFiniteInputs);
    }
}

// ---- quality modes, OS rates, transitions and staging -----------------------------------------------------------------
void quality() {
    Rig r(1);
    if (!r.prepare()) return;
    CHECK(r.e.activeOversampling() == 4 && r.e.activeNewtonCap() == 8, "Studio default 4x cap 8");
    const int L = r.e.latencySamples();
    struct Step { int mode, over, factor, cap; };
    const Step steps[] = {{1, 1, 1, 8}, {1, 2, 2, 8}, {1, 4, 8, 8}, {1, 5, 16, 8}, {2, 0, 16, 12}, {0, 0, 4, 6}, {1, 3, 4, 8}, {2, 3, 4, 12}};
    const Buf tone = sine(1, 2400, 48000, 440, 0.3);
    double prevEnd = 0;
    for (const Step& s : steps) {
        set(r.p, "quality.mode", s.mode); set(r.p, "quality.oversampling", s.over);
        Buf out = r.run(tone, {128});
        CHECK(r.e.activeOversampling() == s.factor && r.e.activeNewtonCap() == s.cap && !r.e.qualityTransitionActive(),
              "quality mode %d override %d -> %dx cap %d (got %dx cap %d)", s.mode, s.over, s.factor, s.cap, r.e.activeOversampling(), r.e.activeNewtonCap());
        CHECK(r.e.latencySamples() == L, "latency constant within the FIR family");
        double jump = std::fabs(out[0][0] - prevEnd);
        for (int i = 1; i < 2400; ++i) jump = std::max(jump, std::fabs(out[0][i] - out[0][i - 1]));
        CHECK(allFinite(out, 2.0) && jump < 0.1, "transition to %dx continuous (max step %g)", s.factor, jump);
        prevEnd = out[0].back();
    }
    // Staged changes: phase family / zero latency / pitch drift need re-prepare; active config unchanged.
    auto staged = [&](const char* id) {
        auto q = r.p;
        set(q, id, 1);
        CHECK(r.e.needsReprepare(q), "%s change must be staged", id);
        Buf out(1, std::vector<double>(256));
        const double* ip[1] = {tone[0].data()};
        double* op[1] = {out[0].data()};
        r.e.process(r.ctx, ip, op, 1, nullptr, 0, 256, q, r.mod);
        engine::TelemetryFrame t{};
        r.e.readTelemetry(t);
        CHECK(t.reprepareRequired && r.e.latencySamples() == L && !t.activeZeroLatency && t.activeFamilyIir == 0, "%s pending vs active", id);
    };
    staged("quality.phase");
    staged("quality.zeroLatency");
    staged("pitchDrift.enabled");
    CHECK(!r.e.needsReprepare(r.p), "quality.mode/oversampling are not staged");
    // Zero latency prepared: 1x regardless of overrides; IIR family declared latency.
    Rig z(1);
    set(z.p, "quality.zeroLatency", 1); set(z.p, "quality.oversampling", 5);
    if (!z.prepare()) return;
    CHECK(z.e.activeOversampling() == 1 && z.e.latencySamples() == 0, "zero latency 1x/0 samples");
    z.run(tone, {128});
    CHECK(z.e.activeOversampling() == 1, "zero latency ignores OS override");
}

// ---- macro / modulation endpoints change the expected internal targets ----------------------------------------------
void macroEndpoints() {
    Rig r(1);
    if (!r.prepare()) return;
    const Buf tone = sine(1, 512, 48000, 440, 0.2);
    r.run(tone, {512});
    const auto base = r.e.effectiveTargets(0);
    CHECK(base.driveDb == 6 && base.ageYears == 0 && std::fabs(base.reservoirCapF - 100e-6) < 1e-18 && base.sagAugmentation == 0 && base.tempcoScale == 1 &&
              base.hfShelfDb == 0 && base.fluxDriveFactor == 1 && base.recoveryConductanceScale == 1,
          "identity modulation leaves base targets");
    core::SlotModulation& m = r.mod.slot[0];
    m.ageYearsMin = 60; m.toleranceSigmaAdd = 0.08; m.leakageAdd = 0.1; m.hfShelfDb = -6; m.ageNoiseDbfs = -66;
    m.reservoirCapDivisor = 4; m.sagMacro = 1; m.driveOffsetDb = 18; m.biasOffset = 0.08; m.fluxDriveFactor = 1.5;
    m.thermalSensitivityScale = 5; m.creativeCutoffOct = 0.15; m.creativeDelayMs = 1.5; m.creativeDelayActive = true;
    m.recoveryTauS = 0.5; m.sidechainMode = 2;
    r.run(tone, {512});
    const auto t = r.e.effectiveTargets(0);
    auto near = [](double a, double b) { return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b)); };
    CHECK(near(t.ageYears, 60), "Age: effective age %g", t.ageYears);
    CHECK(near(t.toleranceSigma, 0.25 / 3.0), "Age: tolerance sigma %g (2%%/3 + 0.08 bounded by 25%% envelope)", t.toleranceSigma);
    CHECK(near(t.leakageFraction, 0.1 + 0.004 * 60), "Age: leakage fraction %g", t.leakageFraction);
    CHECK(near(t.hfShelfDb, -6) && near(t.ageNoiseDbfs, -66) && t.ageNoiseGated, "Age: shelf %g noise %g gated %d", t.hfShelfDb, t.ageNoiseDbfs, t.ageNoiseGated);
    CHECK(near(t.reservoirCapF, 25e-6) && near(t.reservoirCapBaseF, 100e-6), "Sag: C_eff %g", t.reservoirCapF);
    CHECK(near(t.sagAugmentation, 3.0) && t.sidechainMode == 2, "Sag: 3s augmentation %g (Blend)", t.sagAugmentation);
    CHECK(near(t.driveDb, 24) && near(t.biasNormalized, 0.08) && near(t.fluxDriveFactor, 1.5), "Warmth: drive %g bias %g flux %g", t.driveDb,
          t.biasNormalized, t.fluxDriveFactor);
    CHECK(near(t.saturationFluxWb, 0.4 / 1.5), "Warmth: saturation flux %g", t.saturationFluxWb);
    CHECK(near(t.tempcoScale, 5) && near(t.creativeCutoffOct, 0.15), "Drift: tempco %g cutoff %g", t.tempcoScale, t.creativeCutoffOct);
    CHECK(!t.creativeDelayActive, "creative delay inactive unless pitchDrift prepared");
    const double tauNat = 25e-6 * (280.0 / 0.0224) * 0.15;
    CHECK(near(t.recoveryConductanceScale, std::clamp(tauNat / 0.5, 0.05, 20.0)), "Tempo: recovery scale %g", t.recoveryConductanceScale);
    const auto b = r.e.effectiveTargets(1);
    CHECK(b.driveDb == 6 && b.ageYears == 0 && std::fabs(b.reservoirCapF - 100e-6) < 1e-18, "slot B unaffected by slot A modulation");
    m.sidechainMode = 1;
    r.run(tone, {512});
    CHECK(r.e.effectiveTargets(0).sagAugmentation == 0, "External sidechain removes creative main-load augmentation");
    // Internal effect: +18 dB drive offset raises the wet level (no auto-gain).
    Rig a(1), c(1);
    if (!a.prepare()) return;
    c.mod.slot[0].driveOffsetDb = 18; c.mod.slot[1].driveOffsetDb = 18;
    if (!c.prepare()) return;
    const Buf in = sine(1, 4800, 48000, 440, 0.01);
    const double ra = rms(a.run(in)[0], 2400, 4800), rc = rms(c.run(in)[0], 2400, 4800);
    CHECK(20 * std::log10(rc / ra) > 12.0, "Warmth drive offset raises level %g dB", 20 * std::log10(rc / ra));
}

// ---- checkpoint continuation ---------------------------------------------------------------------------------
void checkpoint() {
    auto configure = [](Rig& r) {
        set(r.p, "slot.a.noise.thermalNoise", -90); set(r.p, "slot.a.noise.humLevel", -80); set(r.p, "slot.a.noise.microphonics", 0.3);
        set(r.p, "pitchDrift.enabled", 1); set(r.p, "ab.crossfade", 0.4); set(r.p, "slot.b.stage.activeElement", 1);
        set(r.p, "gainMatch.enabled", 1); set(r.p, "slot.a.filter.lpfEnabled", 1); set(r.p, "analogNoise.enabled", 1);
        r.mod.slot[0].creativeDelayActive = true; r.mod.slot[0].creativeDelayMs = 0.7; r.mod.slot[0].ageNoiseDbfs = -85;
    };
    const Buf in = sine(1, 8000, 48000, 523, 0.35);
    Buf first(1, std::vector<double>(in[0].begin(), in[0].begin() + 4000));
    Buf second(1, std::vector<double>(in[0].begin() + 4000, in[0].end()));
    Rig a(1);
    configure(a);
    if (!a.prepare()) return;
    engine::Checkpoint cp;
    a.e.allocateCheckpoint(cp);
    CHECK(cp.values.size() == a.e.checkpointSize() && !cp.rng.empty(), "checkpoint sizes %zu", cp.values.size());
    a.run(first, {100});
    CHECK(a.e.getCheckpoint(cp), "getCheckpoint");
    const Buf contA = a.run(second, {100});
    Rig b(1);
    configure(b);
    if (!b.prepare()) return;
    CHECK(b.e.setCheckpoint(cp), "setCheckpoint");
    b.ctx = a.ctx;
    const Buf contB = b.run(second, {100});
    CHECK(maxDiff(contA, contB) <= 1e-9, "checkpoint continuation diff %g (T24 limit 1e-6)", maxDiff(contA, contB));
    Rig other(1, 44100);
    configure(other);
    if (!other.prepare()) return;
    CHECK(!other.e.setCheckpoint(cp), "checkpoint for another sample rate rejected");
    engine::Checkpoint bad = cp;
    bad.values[5] = NAN;
    CHECK(!b.e.setCheckpoint(bad), "non-finite checkpoint rejected");
}

// ---- condition overlays and stable component identities ----------------------------------------------------------
void conditions() {
    const Buf in = sine(1, 6000, 48000, 1000, 0.2);
    Rig a(1), b(1), c(1);
    if (!a.prepare() || !b.prepare() || !c.prepare()) return;
    engine::ConditionSet identity{};
    CHECK(b.e.submitConditions(0, identity) && b.e.submitConditions(1, identity), "submit identity");
    const Buf ya = a.run(in), yb = b.run(in);
    CHECK(maxDiff(ya, yb) == 0.0, "identity condition changes nothing (%g)", maxDiff(ya, yb));
    engine::ConditionSet fault{};
    fault.c[int(engine::ComponentId::C_out)].open = true;
    fault.c[int(engine::ComponentId::V1)].valueScale = 0.5;
    CHECK(c.e.submitConditions(0, fault) && c.e.submitConditions(1, fault), "submit fault");
    const Buf yc = c.run(in);
    const double db = 20 * std::log10(rms(yc[0], 3000, 6000) / rms(ya[0], 3000, 6000));
    CHECK(allFinite(yc, 4.0) && std::fabs(db) > 1.0, "open C_out + weak V1 changes level by %g dB", db);
    CHECK(!c.e.submitConditions(2, fault), "slot index validated");
}

// ---- telemetry, supply and thermal behaviour -------------------------------------------------------------------------
void telemetryAndThermal() {
    Rig r(2);
    if (!r.prepare()) return;
    engine::TelemetryFrame t{};
    CHECK(!r.e.readTelemetry(t), "no telemetry before processing");
    r.run(sine(2, 9600, 48000, 80, 0.7), {256});
    CHECK(r.e.readTelemetry(t) && t.generation > 0 && t.activeFactor == 4 && t.latencySamples == r.e.latencySamples(), "telemetry basics");
    const auto& s = t.slot[0];
    const double amb = 298.15;
    CHECK(s.temperatureK[0][int(engine::ComponentId::V1)] > amb + 10 && s.temperatureK[0][int(engine::ComponentId::Chassis)] > amb,
          "warm start: heated V1 %g K", s.temperatureK[0][int(engine::ComponentId::V1)]);
    CHECK(s.railVolts > 200 && s.railVolts < 330 && s.railDcVolts > 200 && s.stageCurrentA[0] > 1e-4, "rail %g V dc %g I %g", s.railVolts, s.railDcVolts,
          s.stageCurrentA[0]);
    double bmax = 0, hmax = 0, rmin = 1e9, rmax = 0;
    for (int k = 0; k < engine::kRingSize; ++k) {
        bmax = std::max(bmax, std::fabs(s.bhRing[k][1])); hmax = std::max(hmax, std::fabs(s.bhRing[k][0]));
        rmin = std::min(rmin, s.railRing[k]); rmax = std::max(rmax, s.railRing[k]);
    }
    CHECK(bmax > 1e-3 && hmax > 0 && rmax > rmin, "B-H ring B %g T H %g A/m; rail ripple %g V", bmax, hmax, rmax - rmin);
    CHECK(t.outPeak[0] > 0 && t.inRms[1] > 0, "levels");
    // External load in External mode sags the rail relative to Main mode.
    auto railWith = [&](double amps, int mode) {
        Rig q(1);
        set(q.p, "quality.oversampling", 1);
        q.mod.slot[0].sidechainMode = mode;
        if (!q.prepare()) return 0.0;
        std::vector<double> ext(24000, amps);
        q.run(sine(1, 24000, 48000, 200, 0.2), {512}, false, &ext);
        engine::TelemetryFrame f{};
        q.e.readTelemetry(f);
        return f.slot[0].railDcVolts;
    };
    const double main = railWith(0.03, 0), external = railWith(0.03, 1);
    CHECK(external < main - 1.0, "external load sag: main %g V external %g V", main, external);
    // Ambient step: temperatures move toward the new ambient, faster with low thermal mass.
    auto rise = [&](double mass) {
        Rig q(1);
        set(q.p, "quality.oversampling", 1); set(q.p, "slot.a.thermal.mass", mass);
        if (!q.prepare()) return 0.0;
        engine::TelemetryFrame f{};
        q.run(sine(1, 480, 48000, 200, 0.1), {480});
        q.e.readTelemetry(f);
        const double t0 = f.slot[0].temperatureK[0][int(engine::ComponentId::R_load)];
        set(q.p, "slot.a.thermal.ambient", 45);
        q.run(sine(1, 48000, 48000, 200, 0.1), {512});
        q.e.readTelemetry(f);
        return f.slot[0].temperatureK[0][int(engine::ComponentId::R_load)] - t0;
    };
    const double fast = rise(0.0), slow = rise(1.0);
    CHECK(fast > 0 && slow > 0 && fast > 4 * slow, "ambient step: low mass %g K vs high mass %g K in 1 s", fast, slow);
}
}  // namespace

int main() {
    struct Section { const char* name; void (*fn)(); };
    const Section sections[] = {{"t20EngineClock", t20EngineClock}, {"macroEndpoints", macroEndpoints}, {"latency", latency},
                                {"mixZeroAndGains", mixZeroAndGains}, {"abUnity", abUnity}, {"containment", containment},
                                {"quality", quality}, {"conditions", conditions}, {"checkpoint", checkpoint},
                                {"partitionInvariance", partitionInvariance}, {"t22Integrated", t22Integrated},
                                {"t23MatchAndClip", t23MatchAndClip}, {"telemetryAndThermal", telemetryAndThermal}};
    for (const Section& sec : sections) {
        const auto t0 = std::chrono::steady_clock::now();
        const int before = g_fail;
        sec.fn();
        const double sec_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%s] %s %.2f s\n", g_fail == before ? "ok" : "FAILED", sec.name, sec_s);
    }
    std::printf(g_fail ? "ENGINE RUNTIME FAILED (%d)\n" : "ENGINE RUNTIME PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
