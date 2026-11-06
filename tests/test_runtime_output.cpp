// CircuitDrift tests: integrated output stage (T22 HPF, T23 gain/blend/clip, containment, host
// bypass, events) through runtime::Engine. Circuits are labelled SYNTHETIC fixtures; no physical
// validation is claimed. Owner: runtime agent.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include "dsp/OutputProtection.h"
#include "runtime/Engine.h"

namespace {
using namespace cd;
using namespace cd::runtime;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)
using Buf = std::vector<std::vector<double>>;
using Timed = std::vector<std::pair<int64_t, ParamEvent>>;

EngineConfig engineCfg(double fs, CircuitConfig circuit, int channels = 1, QualitySettings q = {}) {
    EngineConfig c;
    c.sampleRate = fs; c.maxBlock = 512; c.channels = channels; c.quality = q; c.circuit = circuit;
    c.initial.hpfEnabled = false; c.initial.softClipEnabled = false;
    return c;
}

// Processes x in blocks, delivering timed events at their sample offsets. Returns OR of statuses.
uint32_t run(Engine& eng, const Buf& x, Buf& y, int block = 256, const Timed& ev = {}) {
    const int C = int(x.size());
    const std::size_t N = x[0].size();
    y.assign(std::size_t(C), std::vector<double>(N, 0.0));
    std::vector<ParamEvent> evs;
    std::size_t k = 0;
    uint32_t st = 0;
    for (std::size_t pos = 0; pos < N; pos += std::size_t(block)) {
        const uint32_t n = uint32_t(std::min<std::size_t>(std::size_t(block), N - pos));
        evs.clear();
        while (k < ev.size() && ev[k].first < int64_t(pos + n)) {
            ParamEvent e = ev[k].second; e.offset = uint32_t(ev[k].first - int64_t(pos)); evs.push_back(e); ++k;
        }
        const double* in[2] = {x[0].data() + pos, C > 1 ? x[1].data() + pos : nullptr};
        double* out[2] = {y[0].data() + pos, C > 1 ? y[1].data() + pos : nullptr};
        ProcessBlock b; b.ctx = {eng.active()->circuit().sampleRate(), int64_t(pos), 0, n, false};
        b.buses = {C, in, out}; b.events = evs.data(); b.eventCount = uint32_t(evs.size());
        st |= eng.process(b).status;
    }
    return st;
}

Buf tone(std::size_t n, double amp, double hz, double fs, int C = 1) {
    Buf x(std::size_t(C), std::vector<double>(n, 0.0));
    for (int c = 0; c < C; ++c)
        for (std::size_t i = 0; i < n; ++i) x[std::size_t(c)][i] = amp * std::sin(2 * M_PI * hz * double(i) / fs);
    return x;
}

double ampFit(const std::vector<double>& y, double hz, double fs, std::size_t from, std::size_t len) {
    double s = 0, c = 0;
    for (std::size_t n = from; n < from + len; ++n) { s += y[n] * std::sin(2 * M_PI * hz * double(n) / fs); c += y[n] * std::cos(2 * M_PI * hz * double(n) / fs); }
    return 2 * std::hypot(s, c) / double(len);
}

double rms(const std::vector<double>& y, std::size_t from, std::size_t to) {
    double e = 0; for (std::size_t n = from; n < to; ++n) e += y[n] * y[n];
    return std::sqrt(e / double(to - from));
}

// ---- T22: output HPF through the integrated engine ------------------------------------------
void t22_engine_hpf() {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        auto cfg = engineCfg(fs, syntheticDivider(2.0));
        cfg.initial.mix = 0.0; cfg.initial.hpfEnabled = true;  // dry endpoint: protection still applies
        Engine eng; std::string e;
        CHECK(eng.prepare(cfg, e), "prepare %s", e.c_str());
        Buf y;
        run(eng, tone(std::size_t(4 * fs), 0.5, 15.0, fs), y, 512);
        const double db = 20 * std::log10(ampFit(y[0], 15.0, fs, std::size_t(2 * fs), std::size_t(2 * fs)) / 0.5);
        Engine dc; dc.prepare(cfg, e);
        const int D = dc.latencySamples();
        Buf ydc;
        run(dc, Buf(1, std::vector<double>(std::size_t(2 * fs) + std::size_t(D) + 1, 1.0)), ydc, 512);
        const double resid = 20 * std::log10(std::fabs(ydc[0].back()) + 1e-300);
        std::printf("T22 engine fs %6.0f: 15 Hz %.4f dB (spec -3.0103+-0.05), DC residual after 2 s %.1f dBFS\n", fs, db, resid);
        CHECK(std::fabs(db + 3.0103) <= 0.05, "fs %g corner %g dB", fs, db);
        CHECK(resid < -100.0, "fs %g DC residual %g dBFS", fs, resid);
    }
    const double fs = 48000;
    auto cfg = engineCfg(fs, syntheticDivider(2.0));
    cfg.initial.mix = 0.0;
    // HPF bypass permits DC.
    {
        Engine eng; std::string e; eng.prepare(cfg, e);
        Buf y; run(eng, Buf(1, std::vector<double>(24000, 1.0)), y);
        CHECK(std::fabs(y[0].back() - 1.0) < 1e-3 && eng.active()->output().diagnostics().hpfWeight == 0.0, "HPF off DC %g", y[0].back());
    }
    // 20 ms enable crossfade between continuously running filtered/unfiltered paths.
    {
        cfg.initial.hpfEnabled = true;
        Engine eng; std::string e; eng.prepare(cfg, e);
        const std::size_t t0 = 96000, ramp = 960;
        const int Ds = eng.quality().clipLatency;
        Buf y; run(eng, Buf(1, std::vector<double>(t0 + 4000, 1.0)), y, 300, {{int64_t(t0), {0, ParamTarget::HpfEnabled, 0.0}}});
        double worst = 0;
        for (std::size_t k = 0; k <= ramp + 200; ++k) {
            const double want = std::min(1.0, double(k) / double(ramp));
            worst = std::max(worst, std::fabs(y[0][t0 + std::size_t(Ds) + k] - want));
        }
        std::printf("T22 HPF disable crossfade: max deviation from 20 ms linear ramp %.2e (start %.2e, mid %.4f)\n", worst,
                    y[0][t0 + std::size_t(Ds)], y[0][t0 + std::size_t(Ds) + ramp / 2]);
        CHECK(worst < 2e-3, "HPF crossfade deviation %g", worst);
    }
    // Cutoff smoothing: 20 ms log-frequency ramp, stable, lands exactly on the target.
    {
        Engine eng; std::string e; eng.prepare(cfg, e);
        Buf x = tone(4800, 0.5, 100, fs), y;
        Timed ev = {{100, {0, ParamTarget::HpfCutoffHz, 250.0}}};
        run(eng, Buf(1, std::vector<double>(x[0].begin(), x[0].begin() + 100 + 480)), y, 64, ev);
        const double mid = eng.active()->output().diagnostics().hpfCutoffHz;
        Buf y2; run(eng, Buf(1, std::vector<double>(480, 0.5)), y2, 64);
        const double end = eng.active()->output().diagnostics().hpfCutoffHz;
        std::printf("T22 cutoff ramp 15->250 Hz: at 10 ms %.2f Hz (log-midpoint %.2f), at 20 ms %.6f Hz\n", mid, std::sqrt(15.0 * 250.0), end);
        CHECK(std::fabs(mid / std::sqrt(15.0 * 250.0) - 1) < 0.01 && end == 250.0, "cutoff ramp %g %g", mid, end);
        bool fin = true; for (double v : y[0]) fin = fin && std::isfinite(v);
        CHECK(fin, "cutoff ramp output non-finite");
    }
}

// ---- T23: gain, unity blend, RMS match, clip ---------------------------------------------------
void t23_gain() {
    const double fs = 48000;
    auto measure = [&](double inDb, double trimDb) {
        auto cfg = engineCfg(fs, syntheticDivider(2.0));
        cfg.initial.mix = 0.0; cfg.initial.inputGainDb = inDb; cfg.initial.outputTrimDb = trimDb;
        Engine eng; std::string e; eng.prepare(cfg, e);
        Buf y; run(eng, tone(9600, 0.01, 1000, fs), y);
        return ampFit(y[0], 1000, fs, 4800, 4800);
    };
    const double ref = measure(0, 0);
    double worst = 0;
    for (double g : {-24.0, -12.0, -6.0, 6.0, 12.0, 24.0}) {
        worst = std::max(worst, std::fabs(20 * std::log10(measure(g, 0) / ref) - g));
        worst = std::max(worst, std::fabs(20 * std::log10(measure(0, g) / ref) - g));
    }
    const double out30 = 20 * std::log10(measure(30, -30) / ref);  // clamped to +24/-24
    std::printf("T23 input gain / output trim: max error %.2e dB over +-24 dB; +30/-30 request -> %.3g dB (clamped)\n", worst, out30);
    CHECK(worst <= 0.01 && std::fabs(out30) <= 0.01, "gain error %g", worst);
    // 10 ms one-pole smoothing: 63.2 % of a +12 dB step after one time constant.
    auto cfg = engineCfg(fs, syntheticDivider(2.0));
    cfg.initial.mix = 0.0;
    Engine eng; std::string e; eng.prepare(cfg, e);
    Buf y; run(eng, Buf(1, std::vector<double>(1000 + 480, 0.0)), y, 100, {{1000, {0, ParamTarget::InputGainDb, 12.0}}});
    const double g = eng.active()->output().diagnostics().inputGain, gt = std::pow(10.0, 0.6);
    std::printf("T23 gain smoothing: after 10 ms %.4f of step (one-pole tau 10 ms -> 0.632)\n", (g - 1) / (gt - 1));
    CHECK(std::fabs((g - 1) / (gt - 1) - (1 - std::exp(-1.0))) < 0.005, "gain smoothing %g", (g - 1) / (gt - 1));
}

void t23_unity_blend() {
    const double fs = 48000;
    for (bool autoGain : {false, true}) {
        double worst = 0, ref = 0;
        for (double m : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            auto cfg = engineCfg(fs, syntheticDivider(2.0));  // wet path identical (unity, linear) to dry
            cfg.initial.mix = m; cfg.initial.autoGain = autoGain;
            Engine eng; std::string e; eng.prepare(cfg, e);
            Buf y; run(eng, tone(96000, 0.25, 1000, fs), y, 512);
            const double a = ampFit(y[0], 1000, fs, 48000, 48000);
            if (m == 0.0) ref = a;
            worst = std::max(worst, std::fabs(20 * std::log10(a / ref)));
        }
        std::printf("T23 unity wet/dry (identical paths, auto-gain %s): max deviation %.2e dB\n", autoGain ? "on" : "off", worst);
        CHECK(worst < 0.01, "unity blend deviation %g dB", worst);
    }
}

void t23_rms_match() {
    const double fs = 48000;
    struct Case { const char* name; CircuitConfig c; };
    for (const Case& k : {Case{"divider -6.02 dB", syntheticDivider(1.0)}, Case{"diode clipper", syntheticDiodeClipper()}}) {
        double res[2] = {};
        for (int on = 0; on < 2; ++on) {
            auto cfg = engineCfg(fs, k.c);
            cfg.initial.autoGain = on; cfg.initial.mix = 1.0;
            Engine eng; std::string e; eng.prepare(cfg, e);
            Buf x = tone(std::size_t(6 * fs), 0.5, 220, fs), y;
            run(eng, x, y, 512);
            res[on] = 20 * std::log10(rms(y[0], std::size_t(5 * fs), std::size_t(6 * fs)) / rms(x[0], std::size_t(5 * fs), std::size_t(6 * fs)));
        }
        std::printf("T23 RMS match %-16s: wet-dry %.3f dB without, %.3f dB with auto-gain (after 5 s)\n", k.name, res[0], res[1]);
        CHECK(std::fabs(res[1]) <= 0.5, "%s RMS match %g dB", k.name, res[1]);
    }
    // 50 ms enable ramp.
    auto cfg = engineCfg(fs, syntheticDivider(1.0));
    Engine eng; std::string e; eng.prepare(cfg, e);
    Buf y; run(eng, tone(2400, 0.5, 220, fs), y, 100, {{0, {0, ParamTarget::AutoGain, 1.0}}});  // 50 ms = 2400 samples
    const double wEnd = eng.active()->output().diagnostics().autoGainWeight;
    Engine e2; e2.prepare(cfg, e);
    run(e2, tone(1200, 0.5, 220, fs), y, 100, {{0, {0, ParamTarget::AutoGain, 1.0}}});
    const double wMid = e2.active()->output().diagnostics().autoGainWeight;
    CHECK(wEnd == 1.0 && std::fabs(wMid - 0.5) < 1e-9, "auto-gain ramp end %g mid %g", wEnd, wMid);
}

void t23_soft_clip() {
    const double fs = 48000;
    struct Mode { const char* name; QualitySettings q; };
    QualitySettings studio, m2, m1, zl;
    m2.mode = QualityMode::Manual; m2.manualFactor = 2;
    m1.mode = QualityMode::Manual; m1.manualFactor = 1;
    zl.zeroLatency = true;
    for (const Mode& m : {Mode{"Studio 4x FIR", studio}, Mode{"Draft 2x FIR", m2}, Mode{"Draft 1x", m1}, Mode{"Zero Latency 1x", zl}}) {
        auto cfg = engineCfg(fs, syntheticDivider(2.0), 1, m.q);
        cfg.initial.softClipEnabled = true; cfg.initial.mix = 1.0;
        Engine eng; std::string e;
        CHECK(eng.prepare(cfg, e), "%s prepare %s", m.name, e.c_str());
        const std::size_t N = 48000;
        Buf x(1, std::vector<double>(2 * N));
        for (std::size_t n = 0; n < N; ++n) x[0][n] = 3.0 * double(n) / double(N);                     // slow ramp 0..3
        for (std::size_t n = N; n < 2 * N; ++n) x[0][n] = 2.0 * std::sin(2 * M_PI * 1000 * double(n) / fs); // hot tone
        Buf y; run(eng, x, y, 512);
        const std::size_t D = std::size_t(eng.latencySamples());
        double peak = 0, step = 0, inStep = 0, exact = 0;
        for (std::size_t n = 0; n < 2 * N; ++n) peak = std::max(peak, std::fabs(y[0][n]));
        for (std::size_t n = D + 1; n < N; ++n) {
            step = std::max(step, std::fabs(y[0][n] - y[0][n - 1]));
            inStep = std::max(inStep, std::fabs(x[0][n - D] - x[0][n - D - 1]));
            exact = std::max(exact, std::fabs(y[0][n] - dsp::softClip(x[0][n - D])));
        }
        std::printf("T23 soft clip %-16s (draft %d): output peak %.6f, max step %.3g (input %.3g), ramp vs memoryless curve %.2e, latency %zu\n",
                    m.name, eng.quality().draft, peak, step, inStep, exact, D);
        CHECK(eng.quality().draft == (eng.quality().factor < 4), "draft label");
        // Continuity: no jump larger than the input slope (5 % allowance for FIR ringing at the ramp's
        // onset kink; a discontinuity at the knee would be orders of magnitude larger).
        CHECK(step <= inStep * 1.05 + 1e-9, "%s discontinuity %g", m.name, step);
        // Native rate: the memoryless curve, |y| <= 1. Oversampled: the high-rate signal is bounded by 1;
        // decimated sample peaks overshoot (Gibbs, recorded as a finding; no true-peak ceiling is
        // claimed by the spec). 1.25 is only a containment sanity bound, not a ceiling.
        if (eng.quality().factor == 1) CHECK(peak <= 1.0 && exact <= 1e-12, "%s native-rate clip peak %g err %g", m.name, peak, exact);
        else CHECK(peak <= 1.25 && exact <= 1e-3, "%s oversampled clip peak %g err %g", m.name, peak, exact);
        if (eng.quality().factor > 1) std::printf("T23 FINDING: %s decimated sample peak %.4f > 1 on a 2.0-amplitude 1 kHz tone\n", m.name, peak);
    }
    // Disabled soft clip passes unclipped floating-point output.
    auto cfg = engineCfg(fs, syntheticDivider(2.0));
    cfg.initial.mix = 1.0;
    Engine eng; std::string e; eng.prepare(cfg, e);
    Buf y; run(eng, Buf(1, std::vector<double>(4800, 2.0)), y);
    CHECK(std::fabs(y[0].back() - 2.0) < 1e-3, "soft clip off output %g", y[0].back());
}

// ---- Containment, entry sanitization, recovery ----------------------------------------------
void containment() {
    const double fs = 48000;
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    auto cfg = engineCfg(fs, syntheticDiodeClipper(), 2);
    cfg.initial.hpfEnabled = true; cfg.initial.softClipEnabled = true; cfg.initial.autoGain = true;
    // (a) NaN/Inf input: sanitized at entry; identical to zeros; no persistent poisoning.
    {
        Buf x = tone(9600, 0.5, 300, fs, 2), xz = x;
        x[0][1000] = nan; x[1][2000] = -inf; x[0][3000] = inf;
        xz[0][1000] = 0; xz[1][2000] = 0; xz[0][3000] = 0;
        Engine a, b; std::string e; a.prepare(cfg, e); b.prepare(cfg, e);
        Buf ya, yb;
        const uint32_t st = run(a, x, ya, 128);
        run(b, xz, yb, 128);
        CHECK(ya == yb && a.diagnostics().inputNonFinite == 3 && (st & status::kInputSanitized) && !a.diagnostics().output.fallbackLatched,
              "non-finite input handling");
    }
    // (b) Huge finite input: |y|<=8 clamp, flag, latched 10 ms dry fallback, explicit reset.
    {
        auto c2 = cfg; c2.initial.softClipEnabled = false; c2.initial.mix = 0.0;
        Engine eng; std::string e; eng.prepare(c2, e);
        Buf x = tone(4800, 1e5, 50, fs, 2), y;
        const uint32_t st = run(eng, x, y, 256);
        bool bounded = true; for (auto& ch : y) for (double v : ch) bounded = bounded && std::isfinite(v) && std::fabs(v) <= 8.0;
        const auto d = eng.diagnostics().output;
        std::printf("Containment huge input: clamped %ld, non-finite %ld, fallback %d weight %.2f, status 0x%x\n", d.clampedOutputs,
                    d.nonFiniteOutputs, d.fallbackLatched, d.fallbackWeight, st);
        CHECK(bounded && d.clampedOutputs > 0 && d.fallbackLatched && d.fallbackWeight == 1.0 && (st & status::kContainment), "huge input");
        eng.reset();
        CHECK(!eng.diagnostics().output.fallbackLatched && eng.diagnostics().output.clampedOutputs == d.clampedOutputs, "reset latch");
    }
    // (c) 1e300 bursts: entry range guard keeps every persistent stage finite; recovery after reset
    // matches a fresh engine exactly; without reset output decays to silence.
    {
        Engine eng, fresh; std::string e; eng.prepare(cfg, e); fresh.prepare(cfg, e);
        Buf x = tone(4800, 1e300, 70, fs, 2), y;
        for (std::size_t n = 0; n < 4800; n += 7) x[1][n] = -std::numeric_limits<double>::max();
        run(eng, x, y, 100);
        bool bounded = true; for (auto& ch : y) for (double v : ch) bounded = bounded && std::isfinite(v) && std::fabs(v) <= 8.0;
        Buf ys; run(eng, Buf(2, std::vector<double>(96000, 0.0)), ys, 512);
        const double tail = std::max(std::fabs(ys[0].back()), std::fabs(ys[1].back()));
        const auto d = eng.diagnostics();
        std::printf("Containment 1e300 burst: entry clamps %ld, stage resets %ld, output bounded %d, tail after 2 s silence %.2e\n",
                    d.inputClamped, d.output.stageResets, bounded, tail);
        CHECK(bounded && d.inputClamped > 0 && tail < 1e-6, "1e300 burst containment");
        eng.reset();
        Buf clean = tone(9600, 0.5, 440, fs, 2), y1, y2;
        run(eng, clean, y1, 128); run(fresh, clean, y2, 128);
        CHECK(y1 == y2, "post-reset output differs from fresh engine");
    }
}

// ---- Host bypass: latency-aligned untrimmed dry, 10 ms crossfade, state keeps evolving --------
void host_bypass() {
    const double fs = 48000;
    auto cfg = engineCfg(fs, syntheticDiodeClipper(), 2);
    cfg.initial.inputGainDb = 6; cfg.initial.outputTrimDb = -12; cfg.initial.hpfEnabled = true; cfg.initial.softClipEnabled = true;
    Engine eng; std::string e; eng.prepare(cfg, e);
    const std::size_t t0 = 5000, t1 = 15000, N = 20000;
    Buf x = tone(N, 0.7, 330, fs, 2), y;
    run(eng, x, y, 333, {{int64_t(t0), {0, ParamTarget::HostBypass, 1}}, {int64_t(t1), {0, ParamTarget::HostBypass, 0}}});
    const std::size_t D = std::size_t(eng.latencySamples());
    bool exact = true;
    for (std::size_t n = t0 + 480; n < t1; ++n)
        for (int c = 0; c < 2; ++c) exact = exact && y[std::size_t(c)][n] == x[std::size_t(c)][n - D];
    const double mid = y[0][t0 + 240], want = 0.5 * x[0][t0 + 240 - D];
    const auto d = eng.diagnostics();
    std::printf("Host bypass: exact delayed dry %d (D=%zu), mid-fade %.4f (dry half %.4f), samples %lld, thermal ticks %ld\n", exact, D, mid,
                want, (long long)d.samples, d.circuit.thermalTicks);
    CHECK(exact && d.samples == int64_t(N) && d.circuit.thermalTicks == long(N / 48), "bypass exactness / evolution");
    CHECK(eng.active()->output().diagnostics().bypassWeight == 0.0, "bypass release");
}

// ---- Event handling: bounded order, late offsets, overflow coalescing, in-place buffers -------
void events_and_inplace() {
    const double fs = 48000;
    auto cfg = engineCfg(fs, syntheticDiodeClipper(), 1);
    Engine eng; std::string e; eng.prepare(cfg, e);
    std::vector<double> buf(256, 0.1), out(256);
    std::vector<ParamEvent> evs(5000);
    for (std::size_t k = 0; k < evs.size(); ++k) evs[k] = {uint32_t(k % 300), ParamTarget::Mix, double(k) / 10000.0};
    evs.back() = {9999, ParamTarget::Mix, 0.3};
    const double* in[1] = {buf.data()}; double* o[1] = {out.data()};
    ProcessBlock b; b.ctx = {fs, 0, 0, 256, false}; b.buses = {1, in, o}; b.events = evs.data(); b.eventCount = uint32_t(evs.size());
    const auto r = eng.process(b);
    CHECK((r.status & status::kEventsCoalesced) && eng.active()->output().params().mix == 0.3 && r.frames == 256, "coalescing");
    // In-place: out == in gives the same result as separate buffers.
    Engine a, c; a.prepare(cfg, e); c.prepare(cfg, e);
    Buf x = tone(3000, 0.6, 500, fs), ys;
    run(c, x, ys, 100);
    std::vector<double> io = x[0];
    for (std::size_t pos = 0; pos < io.size(); pos += 100) {
        double* p[1] = {io.data() + pos};
        ProcessBlock ib; ib.ctx = {fs, int64_t(pos), 0, 100, false}; ib.buses = {1, p, p};
        a.process(ib);
    }
    CHECK(io == ys[0], "in-place processing differs");
    // Frames larger than the prepared slice are split internally.
    Engine big, ref; big.prepare(cfg, e); ref.prepare(cfg, e);
    Buf xl = tone(10000, 0.6, 500, fs), yl, yr;
    run(big, xl, yl, 10000); run(ref, xl, yr, 500);
    CHECK(yl == yr, "oversize block split differs");
}

}  // namespace

int main() {
    t22_engine_hpf();
    t23_gain();
    t23_unity_blend();
    t23_rms_match();
    t23_soft_clip();
    containment();
    host_bypass();
    events_and_inplace();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
