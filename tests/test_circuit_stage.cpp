// P03 integration tests: MnaSolver inside the Oversampler (CircuitStage).
// Every circuit is a labelled SYNTHETIC software fixture (antiparallel-diode RC clipper, resistor
// divider). Nothing here is physical validation (G01) and the alias section does NOT claim T21.
// Usage: cd_circuit_stage_tests            -> functional tests + quick alias smoke (4x only)
//        cd_circuit_stage_tests --alias    -> ONLY the full 64x-reference alias re-measurement (slow:
//                                             about 80 s with ASan/UBSan at -O0)
#include "dsp/CircuitStage.h"
#include "dsp/DeviceManifest.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

// ---- allocation trap (test-local; positive control below proves it counts) --------------------
static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_armed{false};
void* operator new(std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace cd::dsp;
int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL line %d: %s | ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (false)

constexpr double kIs = 1e-14, kN = 1.0, kVt = 0.025852;  // fixture.diode defaults (synthetic)

NetElement vsrc(int node, double v) { NetElement e; e.kind = NetElement::Kind::VoltageSource; e.a = node; e.value = v; return e; }
NetElement dev(const char* id, int a, int b, double p0, double p1 = 0, double p2 = 0) {
    NetElement e; e.model = findSyntheticFixture(id); e.a = a; e.b = b; e.param[0] = p0; e.param[1] = p1; e.param[2] = p2; return e;
}
Netlist clipperNet() {  // vs -> 1k -> node2 || (diode, antiparallel diode, 10 nF)
    Netlist n; n.nodes = 2;
    n.elements = {vsrc(1, 0.0), dev("fixture.resistor", 1, 2, 1000.0), dev("fixture.diode", 2, 0, kIs, kN, kVt),
                  dev("fixture.diode", 0, 2, kIs, kN, kVt), dev("fixture.capacitor", 2, 0, 1e-8)};
    return n;
}
Netlist dividerNet() {  // vs -> 1k -> node2 -> 1k -> gnd : gain 0.5
    Netlist n; n.nodes = 2;
    n.elements = {vsrc(1, 0.0), dev("fixture.resistor", 1, 2, 1000.0), dev("fixture.resistor", 2, 0, 1000.0)};
    return n;
}
CircuitStage::Config baseCfg(Netlist net, int factor, double inV, double outV) {
    CircuitStage::Config c;
    c.net = std::move(net);
    c.inputSource = 0; c.outputNode = 2;
    c.inputVoltsPerFs = inV; c.outputVoltsPerFs = outV;
    c.fs = 48000.0;
    c.os = {factor, OsFamily::Fir, 0.45, 100.0};
    c.newton = {8, 1e-7};
    return c;
}
void addThermal(CircuitStage::Config& c, double C, double G) {
    c.thermal.n = 1; c.thermal.capacity[0] = C; c.thermal.toAmbient[0] = G;
}
std::vector<double> multitone(int n, double amp) {
    std::vector<double> x(n);
    for (int i = 0; i < n; ++i)
        x[i] = amp * (std::sin(2 * M_PI * 440.0 * i / 48000.0) + 0.5 * std::sin(2 * M_PI * 3137.0 * i / 48000.0)
                      + 0.25 * std::sin(2 * M_PI * 9001.0 * i / 48000.0));
    return x;
}
bool allFinite(const std::vector<double>& v) { for (double d : v) if (!std::isfinite(d)) return false; return true; }
bool bitEqual(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}
std::vector<double> run(CircuitStage& s, const std::vector<double>& in) {
    std::vector<double> out(in.size());
    s.process(in.data(), out.data(), in.size());
    return out;
}

// ---- 1. prepare validation + latency declaration ---------------------------------------------
void prepareAndLatency() {
    std::string e;
    CircuitStage s;
    CHECK(!s.prepared() && s.latency() == 0, "fresh stage");
    // Unprepared: sanitised pass-through, never garbage, never throws.
    std::vector<double> in = {0.25, std::numeric_limits<double>::quiet_NaN(), -0.5}, out(3);
    s.process(in.data(), out.data(), 3);
    CHECK(out[0] == 0.25 && out[1] == 0.0 && out[2] == -0.5, "unprepared pass-through %g %g %g", out[0], out[1], out[2]);

    {   // 4x FIR: oversampler latency 79.5 base samples -> declared 80, 2 high-rate pad samples.
        CircuitStage a;
        CHECK(a.prepare(baseCfg(dividerNet(), 4, 1.0, 0.5), e), "prepare 4x: %s", e.c_str());
        CHECK(std::fabs(a.oversamplerLatency() - 79.5) < 1e-9 && a.latency() == 80 && a.padHighRateSamples() == 2,
              "4x lat %g declared %d pad %d", a.oversamplerLatency(), a.latency(), a.padHighRateSamples());
    }
    {   // 16x FIR: 83.375 -> 84, pad 10.
        CircuitStage a;
        CHECK(a.prepare(baseCfg(dividerNet(), 16, 1.0, 0.5), e), "prepare 16x: %s", e.c_str());
        CHECK(a.latency() == 84 && a.padHighRateSamples() == 10, "16x declared %d pad %d", a.latency(), a.padHighRateSamples());
    }
    {   // Explicit latency target pads further (e.g. to the max over 1-16x); below the minimum is rejected.
        auto c = baseCfg(dividerNet(), 4, 1.0, 0.5);
        c.latencyTarget = 84;
        CircuitStage a;
        CHECK(a.prepare(c, e) && a.latency() == 84 && a.padHighRateSamples() == 18, "target 84: pad %d", a.padHighRateSamples());
        c.latencyTarget = 79;
        CHECK(!a.prepare(c, e) && a.latency() == 84, "target below minimum must fail and keep prior preparation");
    }
    {   // Rejections; a failed prepare must leave a previously prepared stage processing unchanged.
        CircuitStage a;
        CHECK(a.prepare(baseCfg(dividerNet(), 2, 1.0, 0.5), e), "prepare ok");
        const int lat = a.latency();
        auto c = baseCfg(dividerNet(), 2, 1.0, 0.5);
        auto bad = c; bad.outputNode = 3;                       CHECK(!a.prepare(bad, e), "outputNode");
        bad = c; bad.outputNode = 0;                            CHECK(!a.prepare(bad, e), "outputNode 0");
        bad = c; bad.inputSource = 1;                           CHECK(!a.prepare(bad, e), "input not a voltage source");
        bad = c; bad.inputSource = 9;                           CHECK(!a.prepare(bad, e), "input index");
        bad = c; bad.inputVoltsPerFs = 0;                       CHECK(!a.prepare(bad, e), "inV 0");
        bad = c; bad.outputVoltsPerFs = -1;                     CHECK(!a.prepare(bad, e), "outV <0");
        bad = c; bad.outputVoltsPerFs = std::numeric_limits<double>::infinity(); CHECK(!a.prepare(bad, e), "outV inf");
        bad = c; bad.fs = 0;                                    CHECK(!a.prepare(bad, e), "fs 0");
        bad = c; bad.fadeSeconds = 0;                           CHECK(!a.prepare(bad, e), "fade 0");
        bad = c; bad.os.factor = 3;                             CHECK(!a.prepare(bad, e), "os 3");
        bad = c; bad.newton.maxNewton = 0;                      CHECK(!a.prepare(bad, e), "newton 0");
        bad = c; bad.net.nodes = 40;                            CHECK(!a.prepare(bad, e), "net bounds");
        bad = c; bad.thermal.n = 1;                             CHECK(!a.prepare(bad, e), "thermal C=0");
        CHECK(a.prepared() && a.latency() == lat, "prior preparation retained");
        std::vector<double> x = multitone(64, 0.1), y(64);
        a.process(x.data(), y.data(), 64);
        CHECK(allFinite(y), "still processes");
    }
}

// ---- 2. wet path == oversampler composition; latency alignment ---------------------------------
void integrationComposition() {
    std::string e;
    const double inV = 2.0, outV = 1.0;
    auto cfg = baseCfg(clipperNet(), 4, inV, outV);
    CircuitStage st;
    CHECK(st.prepare(cfg, e), "prepare: %s", e.c_str());
    const auto x = multitone(4096, 0.2);
    const auto y = run(st, x);
    CHECK(st.state() == StageState::Running && st.solverFailures() == 0, "no fallback expected");

    // Independent reference: current Oversampler block API + current MnaSolver, same arithmetic.
    Oversampler os; CHECK(os.prepare(cfg.os, 4096), "os");
    MnaSolver sol; CHECK(sol.prepare(cfg.net, 1.0 / (48000.0 * 4), cfg.newton, e), "sol: %s", e.c_str());
    std::vector<double> hi(4096 * 4), wet(4096);
    os.up(x.data(), hi.data(), 4096);
    for (double& v : hi) { sol.setSource(0, v * inV); sol.step(); v = sol.voltage(2) / outV; }
    // fractional compensation: delay 2 high-rate samples inside the oversampled domain
    std::vector<double> hid(hi.size(), 0.0);
    for (size_t i = 2; i < hi.size(); ++i) hid[i] = hi[i - 2];
    os.down(hid.data(), wet.data(), 4096);
    double worst = 0;
    for (size_t i = 0; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - wet[i]));
    std::printf("INFO composition max |stage - manual| = %g\n", worst);
    CHECK(worst <= 1e-9, "stage vs manual composition %g", worst);
}

void latencyAlignment() {
    std::string e;
    for (int f : {1, 2, 4, 8, 16}) {
        auto cfg = baseCfg(dividerNet(), f, 1.0, 0.5);  // unity small-signal gain, linear
        CircuitStage st; CHECK(st.prepare(cfg, e), "prepare %d: %s", f, e.c_str());
        // impulse: peak must sit exactly at the declared latency with symmetric neighbours.
        std::vector<double> x(512, 0.0); x[0] = 1.0;
        const auto y = run(st, x);
        const int L = st.latency();
        size_t pk = 0; for (size_t i = 0; i < y.size(); ++i) if (std::fabs(y[i]) > std::fabs(y[pk])) pk = i;
        CHECK(int(pk) == L, "f %d impulse peak %zu declared %d", f, pk, L);
        // Peak of the band-limited (0.45 Fs) round-trip impulse response is ~0.984, not 1.
        CHECK(f == 1 ? std::fabs(y[L] - 1.0) < 1e-12 : (y[L] > 0.97 && y[L] < 1.0), "f %d peak %g", f, y[L]);
        if (L > 0) CHECK(std::fabs(y[L - 1] - y[L + 1]) < 1e-9, "f %d symmetry %g", f, y[L - 1] - y[L + 1]);
    }
}

// ---- 3. partition invariance of electrical + thermal timebases --------------------------------
struct Snap { std::vector<double> out; double T; std::uint64_t ticks, hosts; long fails; };
Snap runPartitioned(const CircuitStage::Config& cfg, const std::vector<double>& x, const std::vector<int>& parts) {
    std::string e; CircuitStage s; if (!s.prepare(cfg, e)) { std::printf("prepare fail %s\n", e.c_str()); ++failures; }
    std::vector<double> out(x.size(), -777.0);
    size_t pos = 0, k = 0;
    while (pos < x.size()) {
        size_t n = size_t(parts[k++ % parts.size()]);
        n = std::min(n, x.size() - pos);
        s.process(x.data() + pos, out.data() + pos, n);  // n may be 0: must be a pure no-op
        pos += n;
    }
    s.process(nullptr, nullptr, 0);
    return {out, s.temperatureK(0), s.thermalTicks(), s.hostSamples(), s.solverFailures()};
}
void partitionInvariance() {
    auto cfg = baseCfg(clipperNet(), 4, 3.0, 1.0);
    addThermal(cfg, 0.002, 0.01);
    const auto x = multitone(9000, 0.4);
    const Snap ref = runPartitioned(cfg, x, {int(x.size())});
    CHECK(allFinite(ref.out) && ref.hosts == x.size(), "ref sane");
    CHECK(ref.ticks == x.size() / 48, "ticks %llu expected %zu", (unsigned long long)ref.ticks, x.size() / 48);
    CHECK(ref.T > 298.15, "thermal should warm: %g", ref.T);
    const std::vector<std::vector<int>> plans = {
        {1}, {7}, {48}, {49}, {64, 0, 0, 1, 0, 63}, {0, 5, 0, 0, 977, 0}, {1, 0, 2, 0, 3, 0, 4, 0, 100, 0, 8191},
    };
    for (size_t p = 0; p < plans.size(); ++p) {
        const Snap s = runPartitioned(cfg, x, plans[p]);
        CHECK(bitEqual(s.out, ref.out), "plan %zu output not bit-exact", p);
        CHECK(s.T == ref.T && s.ticks == ref.ticks && s.hosts == ref.hosts && s.fails == ref.fails,
              "plan %zu timebase: T %.17g vs %.17g ticks %llu vs %llu", p, s.T, ref.T, (unsigned long long)s.ticks, (unsigned long long)ref.ticks);
    }
    // Pseudo-random partition including many zero-frame blocks.
    std::vector<int> rnd; std::uint32_t r = 12345;
    for (int i = 0; i < 400; ++i) { r = r * 1664525u + 1013904223u; rnd.push_back(int((r >> 16) % 300) * ((r >> 10) % 3 != 0)); }
    const Snap s = runPartitioned(cfg, x, rnd);
    CHECK(bitEqual(s.out, ref.out) && s.T == ref.T && s.ticks == ref.ticks, "random plan with zero-frame blocks");
    // Non-integer thermal period (44.1 kHz): tick count tracks fs/1000 without block dependence.
    auto c441 = cfg; c441.fs = 44100.0;
    const Snap a = runPartitioned(c441, x, {int(x.size())}), b = runPartitioned(c441, x, {13, 0, 29});
    CHECK(a.ticks == b.ticks && a.T == b.T && bitEqual(a.out, b.out), "44.1k partition");
    CHECK(std::llabs(long(a.ticks) - long(double(x.size()) * 1000.0 / 44100.0)) <= 1, "44.1k ticks %llu", (unsigned long long)a.ticks);
}

// ---- 4. thermal power accounting on DC fixtures ------------------------------------------------
double bisect(double lo, double hi, double V, double R) {
    auto f = [&](double v) { return kIs * (std::exp(v / (kN * kVt)) - 1) - (V - v) / R; };
    for (int i = 0; i < 200; ++i) { const double m = 0.5 * (lo + hi); (f(m) > 0 ? hi : lo) = m; }
    return 0.5 * (lo + hi);
}
void thermalPower() {
    std::string e;
    {   // Divider, 2 V constant: 2 mW total, node-0 heat network C=0.005, G=0.01 -> +0.2 K, tau 0.5 s.
        auto c = baseCfg(dividerNet(), 1, 4.0, 1.0);
        addThermal(c, 0.005, 0.01);
        CircuitStage s; CHECK(s.prepare(c, e), "prepare: %s", e.c_str());
        std::vector<double> x(48000 * 5, 0.5), y(x.size());
        s.process(x.data(), y.data(), x.size());
        CHECK(std::fabs(s.temperatureK(0) - (298.15 + 0.2)) < 1e-4, "divider T %.6f", s.temperatureK(0));
    }
    {   // Series R + diode: dissipation = V*I_src (resistor and diode both counted), 1 V DC.
        Netlist n; n.nodes = 2;
        n.elements = {vsrc(1, 0.0), dev("fixture.resistor", 1, 2, 1000.0), dev("fixture.diode", 2, 0, kIs, kN, kVt)};
        auto c = baseCfg(n, 1, 1.0, 1.0);
        addThermal(c, 0.005, 0.01);
        CircuitStage s; CHECK(s.prepare(c, e), "prepare: %s", e.c_str());
        std::vector<double> x(48000 * 5, 1.0), y(x.size());
        s.process(x.data(), y.data(), x.size());
        const double vd = bisect(0.0, 1.0, 1.0, 1000.0), P = 1.0 * (1.0 - vd) / 1000.0;
        CHECK(std::fabs(s.temperatureK(0) - (298.15 + P / 0.01)) < 2e-4 * P / 0.01 + 1e-6, "diode T %.6f expected %.6f", s.temperatureK(0), 298.15 + P / 0.01);
    }
}

// ---- 5. containment ----------------------------------------------------------------------------
double maxStep(const std::vector<double>& v, size_t a, size_t b) {
    double m = 0; for (size_t i = std::max<size_t>(a, 1); i < b && i < v.size(); ++i) m = std::max(m, std::fabs(v[i] - v[i - 1])); return m;
}
void containment() {
    std::string e;
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    {   // Non-finite input never reaches state or output; stage stays in service.
        auto cfg = baseCfg(clipperNet(), 4, 2.0, 1.0);
        CircuitStage s; CHECK(s.prepare(cfg, e), "prepare");
        auto x = multitone(3000, 0.2);
        x[500] = nan; x[900] = inf; x[901] = -inf;
        const auto y = run(s, x);
        CHECK(allFinite(y), "output finite with NaN/Inf input");
        CHECK(s.inputRepairs() == 3 && s.state() == StageState::Running, "repairs %ld state %d", s.inputRepairs(), int(s.state()));
        // Finite but absurd input (1e300) is clamped to +-1e6 FS; whatever the circuit does, output stays finite.
        CircuitStage g; CHECK(g.prepare(cfg, e), "prepare");
        auto z = multitone(3000, 0.2); z[1500] = 1e300;
        CHECK(allFinite(run(g, z)) && g.inputRepairs() == 1, "1e300 input contained");
    }
    {   // Divergence: Newton cap 1 cannot follow a large burst. Expect 10 ms fade to latency-aligned dry.
        auto cfg = baseCfg(clipperNet(), 4, 50.0, 1.0);
        cfg.newton = {2, 1e-7};
        CircuitStage s; CHECK(s.prepare(cfg, e), "prepare");
        std::vector<double> x(6000, 0.0);
        for (int i = 0; i < 6000; ++i) x[i] = (i < 1500) ? 0.0004 * std::sin(2 * M_PI * 440.0 * i / 48000.0) : 0.9 * std::sin(2 * M_PI * 440.0 * i / 48000.0);
        const auto y = run(s, x);
        CHECK(allFinite(y), "finite");
        CHECK(s.state() == StageState::DryFallback && s.fallbackReason() == FallbackReason::SolverFailures,
              "state %d reason %d fails %ld", int(s.state()), int(s.fallbackReason()), s.solverFailures());
        const int L = s.latency(), fade = s.fadeSamples();
        CHECK(fade == 480, "10 ms at 48 kHz = 480, got %d", fade);
        const size_t tripAt = size_t(s.fallbackSampleIndex());
        std::printf("INFO divergence trip at host sample %zu, step failures %ld\n", tripAt, s.solverFailures());
        CHECK(tripAt >= 1500 && tripAt < 3000, "trip index %zu", tripAt);
        // After the fade the output is exactly the delayed dry input.
        bool exact = true;
        for (size_t i = tripAt + size_t(fade) + 1; i < y.size(); ++i) exact = exact && (i >= size_t(L)) && y[i] == x[i - size_t(L)];
        CHECK(exact, "post-fade output must equal dry delayed by %d", L);
        // No click step: sample-to-sample change in the crossfade stays within signal-scale slew.
        const double slew = std::max(maxStep(y, 0, tripAt), maxStep(x, 0, x.size()));
        CHECK(maxStep(y, tripAt > 2 ? tripAt - 2 : 1, tripAt + size_t(fade) + 4) <= 1.5 * slew + 2.0 / fade,
              "crossfade step %g slew %g", maxStep(y, tripAt - 2, tripAt + size_t(fade) + 4), slew);
        // Re-entry requires explicit reset; reset restores a clean, deterministic running stage.
        s.reset();
        CHECK(s.state() == StageState::Running && s.fallbackReason() == FallbackReason::None && s.hostSamples() == 0, "reset");
        CircuitStage fresh; CHECK(fresh.prepare(cfg, e), "fresh");
        const auto q = multitone(2000, 0.0004);  // tiny signal: converges with 1 iteration
        CHECK(bitEqual(run(s, q), run(fresh, q)), "reset == fresh stage");
    }
    {   // No valid sample yet -> start from delayed dry (instant, no fade).
        auto cfg = baseCfg(clipperNet(), 4, 1.0e7, 1.0);
        cfg.newton = {1, 1e-7};
        CircuitStage s; CHECK(s.prepare(cfg, e), "prepare");
        std::vector<double> x(1000); for (int i = 0; i < 1000; ++i) x[i] = 0.9 * std::sin(2 * M_PI * 440.0 * (i + 1) / 48000.0);
        const auto y = run(s, x);
        CHECK(s.state() == StageState::DryFallback && s.fallbackReason() == FallbackReason::NoValidSample, "reason %d", int(s.fallbackReason()));
        bool ok = true; const int L = s.latency();
        for (size_t i = 0; i < y.size(); ++i) ok = ok && y[i] == (i >= size_t(L) ? x[i - size_t(L)] : 0.0);
        CHECK(ok, "starts from delayed dry");
    }
    {   // Over-amplitude (>8): clamp to +-8, same 10 ms fallback.
        auto cfg = baseCfg(dividerNet(), 4, 1.0, 0.01);  // 0.5 V / 0.01 = 50 full scale
        CircuitStage s; CHECK(s.prepare(cfg, e), "prepare");
        std::vector<double> x(4000, 0.0); for (int i = 1000; i < 4000; ++i) x[i] = 0.9 * std::sin(2 * M_PI * 440.0 * i / 48000.0);
        const auto y = run(s, x);
        double mx = 0; for (double v : y) mx = std::max(mx, std::fabs(v));
        CHECK(mx <= 8.0 && allFinite(y), "clamped max %g", mx);
        CHECK(s.state() == StageState::DryFallback && s.fallbackReason() == FallbackReason::Overamplitude, "state %d reason %d", int(s.state()), int(s.fallbackReason()));
    }
    {   // Non-finite from the circuit path (output scale overflows double): contained, finite, fallback.
        auto cfg = baseCfg(dividerNet(), 4, 4.0, 1e-308);
        CircuitStage s; CHECK(s.prepare(cfg, e), "prepare: %s", e.c_str());
        std::vector<double> x(4000); for (int i = 0; i < 4000; ++i) x[i] = 0.9 * std::sin(2 * M_PI * 440.0 * i / 48000.0);
        const auto y = run(s, x);
        CHECK(allFinite(y), "finite output");
        CHECK(s.state() == StageState::DryFallback, "state %d", int(s.state()));
        double mx = 0; for (double v : y) mx = std::max(mx, std::fabs(v));
        CHECK(mx <= 8.0, "bounded %g", mx);
    }
}

// ---- 6. no allocation / lock / I/O in process (allocation trap with positive control) -------------
void noAllocation() {
    std::string e;
    auto cfg = baseCfg(clipperNet(), 4, 50.0, 1.0);
    cfg.newton = {2, 1e-7};
    addThermal(cfg, 0.002, 0.01);
    CircuitStage s; CHECK(s.prepare(cfg, e), "prepare");
    std::vector<double> x(6000), y(6000);
    for (int i = 0; i < 6000; ++i) x[i] = (i < 1500) ? 0.0004 * std::sin(2 * M_PI * 440.0 * i / 48000.0) : 0.9 * std::sin(2 * M_PI * 440.0 * i / 48000.0);
    g_allocs = 0; g_armed = true;
    { volatile auto* leak = new double[4]; delete[] leak; }  // positive control
    const long control = g_allocs.load();
    g_allocs = 0;
    s.process(x.data(), y.data(), 1500);       // Running
    s.process(x.data() + 1500, y.data() + 1500, 3000);  // Fading -> DryFallback
    s.process(x.data() + 4500, y.data() + 4500, 1500);  // DryFallback
    s.process(nullptr, nullptr, 0);
    s.reset();
    s.process(x.data(), y.data(), 600);
    g_armed = false;
    CHECK(control == 1, "allocation trap positive control %ld", control);
    CHECK(g_allocs.load() == 0, "process/reset allocated %ld times", g_allocs.load());
    CHECK(noexcept(s.process(x.data(), y.data(), 1)) && noexcept(s.reset()), "noexcept contract");
}

// ---- 6b. IIR family: nominal latency only; must stay finite and in-band unity gain ----------------
void iirFamily() {
    std::string e;
    auto cfg = baseCfg(dividerNet(), 4, 1.0, 0.5);
    cfg.os.family = OsFamily::Iir;
    CircuitStage s; CHECK(s.prepare(cfg, e), "iir prepare: %s", e.c_str());
    CHECK(s.latency() >= 0 && s.latency() == int(std::ceil(s.oversamplerLatency() - 1e-9)), "iir declared %d nominal %g", s.latency(), s.oversamplerLatency());
    std::vector<double> x(4800), y;
    for (int i = 0; i < 4800; ++i) x[i] = 0.5 * std::sin(2 * M_PI * 1000.0 * i / 48000.0);
    y = run(s, x);
    double pk = 0; for (size_t i = 2400; i < y.size(); ++i) pk = std::max(pk, std::fabs(y[i]));
    CHECK(allFinite(y) && std::fabs(pk - 0.5) < 0.01 && s.state() == StageState::Running, "iir peak %g", pk);
}

// ---- 7. alias re-measurement on the CIRCUIT (synthetic, informational; no T21 claim) -------------
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> w = std::polar(1.0, -2 * M_PI / double(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> wk = 1;
            for (size_t k = 0; k < len / 2; ++k, wk *= w) {
                const auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
        }
    }
}
constexpr int kN_FFT = 16384;
struct AliasRun { std::vector<double> p; long failures; bool finite; };
AliasRun circuitSpectrum(int factor, int maxNewton, double amp, double inV, int stopbandDb = 100) {
    const int bins[5] = {7 * 37, 7 * 101, 7 * 233, 7 * 419, 7 * 677};
    auto cfg = baseCfg(clipperNet(), factor, inV, 1.0);
    cfg.os.stopbandDb = stopbandDb;
    cfg.newton = {maxNewton, 1e-7};
    std::string e; CircuitStage s;
    if (!s.prepare(cfg, e)) { std::printf("alias prepare failed: %s\n", e.c_str()); ++failures; return {}; }
    const int N = kN_FFT, total = 3 * N, B = 256;
    std::vector<double> in(B), out(B), cap;
    for (int start = 0; start < total; start += B) {
        for (int i = 0; i < B; ++i) {
            double x = 0;
            for (int k = 0; k < 5; ++k) x += amp * std::sin(2 * M_PI * double(bins[k]) * double(start + i) / N);
            in[i] = x;
        }
        s.process(in.data(), out.data(), B);
        for (int i = 0; i < B; ++i) if (start + i >= 2 * N) cap.push_back(out[i]);
    }
    std::vector<std::complex<double>> X(cap.begin(), cap.end());
    fft(X);
    AliasRun r; r.p.resize(N / 2);
    for (int k = 0; k < N / 2; ++k) r.p[k] = std::norm(X[k]);
    r.failures = s.solverFailures();
    r.finite = s.state() == StageState::Running;
    return r;
}
void aliasMeasurement(bool full) {
    const double df = 48000.0 / kN_FFT;
    const int k0 = int(std::ceil(20.0 / df)), k1 = int(20000.0 / df);
    auto unwanted = [&](const std::vector<double>& p) { double e = 0; for (int k = k0; k <= k1; ++k) if (k % 7) e += p[k]; return e; };
    auto inband = [&](const std::vector<double>& p) { double e = 0; for (int k = k0; k <= k1; ++k) e += p[k]; return e; };
    struct Stim { const char* name; double amp; };
    // Same five-tone stimulus as T21 (nominal A=0.1, hot A=0.3); drive is the circuit's 2 V/full-scale
    // input scale instead of the tanh gain. Synthetic antiparallel-diode RC fixture, not hardware.
    const double inV = 2.0;
    for (const Stim st : {Stim{"nominal", 0.1}, Stim{"hot", 0.3}}) {
        if (!full) {   // quick smoke: 4x must run clean and finite on the circuit
            const auto r = circuitSpectrum(4, 8, st.amp, inV);
            CHECK(r.finite && r.failures == 0, "%s 4x circuit finite/no failures (%ld)", st.name, r.failures);
            continue;
        }
        const auto ref = circuitSpectrum(64, 12, st.amp, inV, 140);
        const double eRef = inband(ref.p);
        const double refSelf = 10 * std::log10(unwanted(ref.p) / eRef + 1e-300);
        std::printf("CIRCUIT-ALIAS %-7s 64x reference (maxNewton 12) self-alias %8.2f dB, solver failures %ld\n", st.name, refSelf, ref.failures);
        CHECK(ref.finite && ref.failures == 0, "%s reference run clean", st.name);
        CHECK(refSelf <= -120.0, "%s reference self-alias %g dB (measurement validity)", st.name, refSelf);
        struct Mode { const char* name; int f, newton; double spec; };
        for (const Mode m : {Mode{"Studio 4x FIR (Newton 8)", 4, 8, -70.0}, Mode{"Render 16x FIR (Newton 12)", 16, 12, -85.0}}) {
            const auto r = circuitSpectrum(m.f, m.newton, st.amp, inV);
            const double a = 10 * std::log10(unwanted(r.p) / eRef + 1e-300);
            std::printf("CIRCUIT-ALIAS %-7s %-26s alias %8.2f dB (proposed %.0f dB)%s, solver failures %ld\n", st.name, m.name, a, m.spec,
                        a > m.spec ? " FINDING: exceeds proposed target" : "", r.failures);
            CHECK(r.finite && r.failures == 0, "%s %s must run without solver failures to be a valid measurement", st.name, m.name);
            // Deliberately no alias threshold CHECK: T21 on the circuit stays Partial; misses stay visible above.
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    bool full = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--alias") == 0) full = true;
    if (full) {
        aliasMeasurement(true);
        std::printf("circuit stage alias run: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    prepareAndLatency();
    integrationComposition();
    latencyAlignment();
    partitionInvariance();
    thermalPower();
    containment();
    noAllocation();
    iirFamily();
    aliasMeasurement(false);
    std::printf("circuit stage tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
