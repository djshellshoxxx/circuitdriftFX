// CircuitDrift tests: performance-equivalence and soft-clip-off bypass (spec 01 "Output soft-clip
// protection", "Budgets and acceptance"; T21/T23 neighbourhood, UX-10). Owner: perf agent.
//
//  * PolyphaseHalfband2x::upBlock/downBlock are bit-identical (outputs AND dynamic state) to the
//    per-sample path for every block partition (the interleaved-sum optimisation changes no bit).
//  * With the soft clip fully off the output stage is a PURE INTEGER DELAY of the declared latency
//    (no resampling ripple/band-limit): at mix 0 with HPF off the output equals the input delayed
//    by exactly latencySamples(), bit for bit, for FIR and IIR cascades; the declared latency is
//    identical to the clip-on engine; re-enabling re-primes the oversampler so the enable has no
//    transient; snapshots taken while the bypass runs restore exactly.
// All fixtures are SYNTHETIC (no physical validation, G01 open).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "dsp/Oversampling.h"
#include "runtime/Engine.h"

namespace {
using namespace cd;
using namespace cd::runtime;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)
using Buf = std::vector<std::vector<double>>;
using Timed = std::vector<std::pair<int64_t, ParamEvent>>;

struct Rng {  // deterministic LCG, no globals
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    double next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return double(std::int64_t(s >> 11)) / double(1ull << 52) - 1.0; }
};

// ---- block FIR == per-sample FIR (bit-exact, outputs and state) ------------------------------
void fir_block_equivalence() {
    dsp::PolyphaseHalfband2x a, b;
    CHECK(a.design(0.225, 110.0) && b.design(0.225, 110.0), "design");
    b.reserveBlock(1024);
    Rng rng(7);
    const int partitions[] = {1, 3, 4, 5, 17, 64, 127, 1000};
    for (int mode = 0; mode < 2; ++mode) {  // 0: up, 1: down
        a.reset(); b.reset();
        int total = 0;
        for (int round = 0; round < 6; ++round)
            for (int n : partitions) {
                std::vector<double> in(std::size_t(2 * n)), ra(std::size_t(2 * n)), rb(std::size_t(2 * n));
                for (double& v : in) v = rng.next();
                if (mode == 0) {
                    for (int i = 0; i < n; ++i) a.up(in[std::size_t(i)], ra.data() + 2 * i);
                    b.upBlock(in.data(), rb.data(), n);
                    CHECK(std::memcmp(ra.data(), rb.data(), std::size_t(2 * n) * sizeof(double)) == 0, "up block n=%d differs", n);
                } else {
                    for (int i = 0; i < n; ++i) ra[std::size_t(i)] = a.down(in.data() + 2 * i);
                    b.downBlock(in.data(), rb.data(), n);
                    CHECK(std::memcmp(ra.data(), rb.data(), std::size_t(n) * sizeof(double)) == 0, "down block n=%d differs", n);
                }
                std::vector<double> sa(std::size_t(a.stateSize())), sb(std::size_t(b.stateSize()));
                a.getState(sa.data()); b.getState(sb.data());
                CHECK(std::memcmp(sa.data(), sb.data(), sa.size() * sizeof(double)) == 0, "state after n=%d differs (mode %d)", n, mode);
                total += n;
            }
        CHECK(total > 0, "ran");
    }
    // Over-reserve and unprepared fall back safely (zero-frame, n > reserved).
    dsp::PolyphaseHalfband2x c;
    c.design(0.225, 110.0);
    std::vector<double> in(64, 0.5), out(128), ref(128);
    c.upBlock(in.data(), out.data(), 64);
    dsp::PolyphaseHalfband2x d;
    d.design(0.225, 110.0);
    for (int i = 0; i < 64; ++i) d.up(0.5, ref.data() + 2 * i);
    CHECK(std::memcmp(out.data(), ref.data(), 128 * sizeof(double)) == 0, "no-reserve fallback");
    c.upBlock(in.data(), out.data(), 0);   // zero frames: no effect
    dsp::PolyphaseHalfband2x unprepared;
    unprepared.upBlock(in.data(), out.data(), 8);
    CHECK(out[0] == 0.0 && out[1] == 0.0, "unprepared stage emits zero");
    std::printf("PERF block FIR == per-sample FIR: bit-exact outputs and state over partitions {1,3,4,5,17,64,127,1000}\n");
}

// ---- engine helpers ------------------------------------------------------------------------
EngineConfig cfgOf(QualitySettings q, bool clip, double mix = 0.0, int channels = 2) {
    EngineConfig c;
    c.sampleRate = 48000; c.maxBlock = 256; c.channels = channels; c.quality = q; c.circuit = syntheticDivider(2.0);
    c.initial.hpfEnabled = false; c.initial.softClipEnabled = clip; c.initial.mix = mix;
    return c;
}

uint32_t run(Engine& eng, const Buf& x, Buf& y, int block, const Timed& ev = {}) {
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
        ProcessBlock b; b.ctx = {48000.0, int64_t(pos), 0, n, false};
        b.buses = {C, in, out}; b.events = evs.data(); b.eventCount = uint32_t(evs.size());
        st |= eng.process(b).status;
    }
    return st;
}

Buf signal(std::size_t n, double amp, int C, std::uint64_t seed, double noise = 0.1) {
    Buf x(std::size_t(C), std::vector<double>(std::size_t(n), 0.0));
    Rng rng(seed);
    for (int c = 0; c < C; ++c)
        for (std::size_t i = 0; i < n; ++i) {
            const double t = double(i) / 48000.0;
            x[std::size_t(c)][i] = amp * (0.6 * std::sin(2 * M_PI * (110.0 + 63.0 * c) * t) + 0.3 * std::sin(2 * M_PI * 9100.0 * t) + noise * rng.next());
        }
    return x;
}

QualitySettings quality(QualityMode m, int factor = 1, dsp::OsFamily fam = dsp::OsFamily::Fir) {
    QualitySettings q;
    q.mode = m; q.manualFactor = factor; q.family = fam;
    return q;
}

// ---- soft clip off == pure delay of the declared latency ---------------------------------------
void bypass_is_pure_delay() {
    struct Case { const char* name; QualitySettings q; };
    const Case cases[] = {{"Studio 4x FIR", quality(QualityMode::Studio)},
                          {"Eco 4x FIR", quality(QualityMode::Eco)},
                          {"Render 16x FIR", quality(QualityMode::Render)},
                          {"Manual 2x FIR", quality(QualityMode::Manual, 2)},
                          {"Manual 8x IIR", quality(QualityMode::Manual, 8, dsp::OsFamily::Iir)}};
    for (const Case& c : cases) {
        Engine off, on;
        std::string e;
        CHECK(off.prepare(cfgOf(c.q, false), e) && on.prepare(cfgOf(c.q, true), e), "%s prepare %s", c.name, e.c_str());
        const int L = off.latencySamples();
        CHECK(L == on.latencySamples() && L > 0, "%s declared latency must not depend on the clip switch (off %d on %d)", c.name, L, on.latencySamples());
        const std::size_t N = 6000;
        // Full-band input (tones + white noise, which includes content above the oversampler's protected
        // band) for the bypass; band-limited tones for the clip-on comparison.
        const Buf x = signal(N, 0.5, 2, 11);
        const Buf xt = signal(N, 0.5, 2, 11, 0.0);
        Buf yOff, yOn;
        run(off, x, yOff, 100);
        run(on, xt, yOn, 100);
        double dOff = 0, dOn = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (std::size_t i = std::size_t(L); i < N; ++i) {
                dOff = std::max(dOff, std::fabs(yOff[std::size_t(ch)][i] - x[std::size_t(ch)][i - std::size_t(L)]));
                if (i >= 1000) dOn = std::max(dOn, std::fabs(yOn[std::size_t(ch)][i] - xt[std::size_t(ch)][i - std::size_t(L)]));  // past the filter start-up
            }
        std::printf("PERF soft clip off, mix 0, HPF off, %-15s L=%3d: max |out - delayed in| = %.3e (clip on: %.3e)\n", c.name, L, dOff, dOn);
        CHECK(dOff == 0.0, "%s: clip-off output must equal the latency-aligned input exactly (%.3e)", c.name, dOff);
        // FIR cascades are linear phase: the clip-on round trip is the delay plus a <= -100 dB ripple
        // (IIR halfbands have frequency-dependent phase, so no pure-delay comparison is made for them).
        if (c.q.family == dsp::OsFamily::Fir) CHECK(dOn < 1e-5, "%s: clip-on resampling error unexpectedly large (%.3e)", c.name, dOn);
        // Block-partition independence of the bypass path.
        Engine off2;
        off2.prepare(cfgOf(c.q, false), e);
        Buf y2;
        run(off2, x, y2, 37);
        CHECK(yOff == y2, "%s: bypass differs between block sizes 100 and 37", c.name);
    }
}

// ---- re-enable is primed (no transient) and disable returns to the pure delay ---------------------
void enable_disable_transitions() {
    const QualitySettings q = quality(QualityMode::Studio);
    const std::size_t N = 24000, T = 8000, T2 = 16000;
    // Low level: the clip curve is the identity (|x| <= 0.9), so an exactly primed oversampler makes
    // the enable crossfade invisible; an un-primed one would ramp up from silence.
    const Buf x = signal(N, 0.4, 2, 5);
    Engine always, toggled;
    std::string e;
    always.prepare(cfgOf(q, true), e);
    toggled.prepare(cfgOf(q, false), e);
    Buf ya, yt;
    run(always, x, ya, 128);
    run(toggled, x, yt, 128, {{int64_t(T), {0, ParamTarget::SoftClipEnabled, 1.0}}, {int64_t(T2), {0, ParamTarget::SoftClipEnabled, 0.0}}});
    const int L = toggled.latencySamples();
    double worstEnable = 0, worstOff = 0;
    for (int ch = 0; ch < 2; ++ch) {
        for (std::size_t i = T + std::size_t(L); i < T2 + std::size_t(L); ++i)
            worstEnable = std::max(worstEnable, std::fabs(yt[std::size_t(ch)][i] - ya[std::size_t(ch)][i]));
        // 20 ms fade-out, then the pure delay again.
        for (std::size_t i = T2 + std::size_t(L) + 2000; i < N; ++i)
            worstOff = std::max(worstOff, std::fabs(yt[std::size_t(ch)][i] - x[std::size_t(ch)][i - std::size_t(L)]));
    }
    std::printf("PERF clip enable at %zu: max deviation from an always-on engine %.3e; after disable vs delayed input %.3e\n", T, worstEnable, worstOff);
    CHECK(worstEnable <= 4e-16, "enable must be exactly primed (deviation %.3e)", worstEnable);
    CHECK(worstOff == 0.0, "disabled again: pure delay (deviation %.3e)", worstOff);
}

// ---- snapshots while the bypass runs restore exactly ----------------------------------------------
void bypass_snapshot_roundtrip() {
    const QualitySettings q = quality(QualityMode::Studio);
    const std::size_t N = 6000, cut = 3001;
    const Buf x = signal(N, 0.5, 2, 3);
    Engine a, b;
    std::string e;
    a.prepare(cfgOf(q, false), e);
    b.prepare(cfgOf(q, false), e);
    Buf ya(2, std::vector<double>(N)), yb(2, std::vector<double>(N));
    Buf head(2), tail(2);
    for (int c = 0; c < 2; ++c) { head[std::size_t(c)].assign(x[std::size_t(c)].begin(), x[std::size_t(c)].begin() + std::ptrdiff_t(cut)); }
    Buf y0;
    run(a, head, y0, 64);
    std::vector<std::byte> snap(a.snapshotBytes());
    CHECK(a.captureState(snap) == snap.size() && !snap.empty(), "capture");
    CHECK(b.loadState(snap), "load");
    // Continue both from the same snapshot over the rest of the input.
    for (int c = 0; c < 2; ++c) tail[std::size_t(c)].assign(x[std::size_t(c)].begin() + std::ptrdiff_t(cut), x[std::size_t(c)].end());
    Buf ta, tb;
    run(a, tail, ta, 100);
    run(b, tail, tb, 100);
    CHECK(ta == tb, "restored engine diverges from the original");
    // A restored bypass engine keeps outputting the delayed input (no dropout after the load).
    const int L = a.latencySamples();
    double worst = 0;
    for (std::size_t i = std::size_t(L); i < tail[0].size(); ++i) {
        const std::size_t src = cut + i;   // absolute sample index of the output position (starts at `cut`)
        if (src < std::size_t(L)) continue;
        worst = std::max(worst, std::fabs(tb[0][i] - x[0][src - std::size_t(L)]));
    }
    std::printf("PERF snapshot taken during bypass restores exactly; restored output vs delayed input %.3e\n", worst);
    CHECK(worst == 0.0, "restored bypass output must still equal the delayed input (%.3e)", worst);
    // Corrupt payloads are rejected without effect.
    std::vector<std::byte> bad = snap;
    bad.resize(bad.size() - 8);
    CHECK(!b.loadState(bad), "short payload must be rejected");
}

// ---- sad paths ----------------------------------------------------------------------------------
void bypass_sad_paths() {
    Engine eng;
    std::string e;
    eng.prepare(cfgOf(quality(QualityMode::Studio), false), e);
    Buf x = signal(2000, 0.5, 2, 1);
    x[0][500] = std::numeric_limits<double>::quiet_NaN();
    x[1][700] = std::numeric_limits<double>::infinity();
    x[0][900] = 1e300;
    Buf y;
    run(eng, x, y, 77);
    bool finite = true;
    for (auto& ch : y) for (double v : ch) finite = finite && std::isfinite(v) && std::fabs(v) <= 8.0;
    CHECK(finite, "hostile input must leave finite, contained output with the bypass active");
    const double* in[2] = {x[0].data(), x[1].data()};
    double* out[2] = {y[0].data(), y[1].data()};
    ProcessBlock b; b.ctx = {48000.0, 0, 0, 0, false};
    b.buses = {2, in, out};
    eng.process(b);   // zero frames: must not crash or advance
    CHECK(true, "zero-frame block");
}
}  // namespace

int main() {
    fir_block_equivalence();
    bypass_is_pure_delay();
    enable_disable_transitions();
    bypass_snapshot_roundtrip();
    bypass_sad_paths();
    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("runtime_perf: all checks passed\n");
    return 0;
}
