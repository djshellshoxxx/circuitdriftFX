// CircuitDrift tests: Engine wiring (resume packet 1) — TransactionQueue events, macros (T06 at
// engine level), blueprint graph install/crossfade/retire, A/B SlotEngine, snapshots, RtTrap.
// Every circuit is a labelled SYNTHETIC fixture (fixture-v1); nothing here validates hardware (G01).
// Owner: runtime/integration agent.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>
#include "blueprint/Compiler.h"
#include "control/Macros.h"
#include "core/ParameterRegistry.h"
#include "runtime/Engine.h"
#include "runtime/ProcessorGraph.h"
#include "runtime/SlotEngine.h"
#include "state/SnapshotExchange.h"
#include "state/TransactionQueue.h"
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::runtime;
using cd::state::TransactionEvent;
using cd::state::TransactionQueue;
using cd::test::RtTrapScope;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

long g_rtAllocs = 0, g_rtFrees = 0;   // accumulated over every audio call under the trap
struct Trap {
    RtTrapScope s;
    ~Trap() { g_rtAllocs += s.counts().allocations; g_rtFrees += s.counts().deallocations; }
};

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }   // [-1,1)
    double unit() { return double(u()) / 4294967296.0; }       // [0,1)
};

using Buf = std::vector<std::vector<double>>;   // [channel][sample]

Buf makeInput(int channels, std::size_t n, uint64_t seed, double amp = 0.4) {
    Buf x(static_cast<std::size_t>(channels), std::vector<double>(n));
    Lcg g{seed};
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::sin(2.0 * M_PI * 440.0 * double(i) / 48000.0);
        for (int c = 0; c < channels; ++c) x[std::size_t(c)][i] = amp * (0.7 * s + 0.3 * g.d());
    }
    return x;
}

Buf sine(int channels, std::size_t n, double fs, double hz, double amp) {
    Buf x(static_cast<std::size_t>(channels), std::vector<double>(n));
    for (std::size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) x[std::size_t(c)][i] = amp * std::sin(2.0 * M_PI * hz * double(i) / fs);
    return x;
}

struct TimedEvent { std::size_t t; uint32_t hostId; double norm; };

// Renders `in` through `eng` with the queue: calls cycle through `parts` (0 = zero-frame call);
// events whose absolute time falls in a call are pushed with call-relative offsets.
Buf renderQueued(Engine& eng, TransactionQueue& q, const Buf& in, const std::vector<uint32_t>& parts,
                 const std::vector<TimedEvent>& tl, double fs = 48000) {
    const int C = int(in.size());
    const std::size_t N = in[0].size();
    Buf out(std::size_t(C), std::vector<double>(N, 0.0));
    std::size_t pos = 0, ev = 0, k = 0;
    while (pos < N) {
        const uint32_t f = uint32_t(std::min<std::size_t>(parts[k++ % parts.size()], N - pos));
        Trap trap;
        while (ev < tl.size() && (f == 0 ? tl[ev].t == pos : tl[ev].t < pos + f)) {
            q.push(TransactionEvent::param(tl[ev].hostId, tl[ev].norm, uint32_t(tl[ev].t - pos)));
            ++ev;
        }
        const double* ip[2] = {};
        double* op[2] = {};
        for (int c = 0; c < C; ++c) { ip[c] = in[std::size_t(c)].data() + pos; op[c] = out[std::size_t(c)].data() + pos; }
        ProcessContext ctx{fs, int64_t(pos), 0, f, false};
        eng.processQueued(ctx, AudioBuses{C, ip, op}, q);
        pos += f;
    }
    return out;
}

double peakDiff(const Buf& a, const Buf& b, std::size_t from = 0) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = from; i < a[c].size(); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}

bool allFinite(const Buf& a) {
    for (const auto& ch : a) for (double v : ch) if (!std::isfinite(v)) return false;
    return true;
}

double norm(uint32_t hostId, double plain) { return core::toNormalized(*core::findByHostId(hostId), plain); }

EngineConfig baseConfig(int channels = 2, double fs = 48000, int maxBlock = 512) {
    EngineConfig c;
    c.sampleRate = fs; c.maxBlock = maxBlock; c.channels = channels;
    c.quality.mode = QualityMode::Studio;
    c.circuit = syntheticDiodeClipper();
    c.initial.autoGain = true;
    return c;
}

// Linear unity-gain fixture with protection stages out of the way (macro endpoint measurements).
EngineConfig linearConfig() {
    EngineConfig c = baseConfig(1);
    c.circuit = syntheticDivider(2.0);
    c.initial.autoGain = false; c.initial.softClipEnabled = false; c.initial.hpfEnabled = false;
    return c;
}

std::vector<TimedEvent> randomTimeline(std::size_t n, std::size_t count, uint64_t seed) {
    const uint32_t ids[] = {1000, 1001, 1002, 1003, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1018, 1028, 1040};
    Lcg g{seed};
    std::vector<TimedEvent> tl;
    for (std::size_t k = 0; k < count; ++k) {
        const uint32_t id = ids[g.u() % (sizeof ids / sizeof ids[0])];
        double v = g.unit();
        if (id == 1005 || id == 1006) v = norm(id, -6.0 + 12.0 * g.unit());   // keep gains moderate
        tl.push_back({std::size_t(g.u() % n), id, v});
    }
    std::stable_sort(tl.begin(), tl.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.t < b.t; });
    return tl;
}

// ---------------------------------------------------------------------------------------------
void partitionInvariance() {
    const std::size_t N = 24000;
    const Buf in = makeInput(2, N, 7);
    const auto tl = randomTimeline(N, 60, 11);
    std::string e;
    Buf ref;
    double worst = 0;
    bool bitIdentical = true;
    for (uint32_t p : {512u, 1u, 17u, 64u, 127u, 8192u}) {
        Engine eng;
        CHECK(eng.prepare(baseConfig(), e), "prepare %s", e.c_str());
        TransactionQueue q;
        // Zero-frame calls interleaved: they apply due events only and advance nothing.
        const std::vector<uint32_t> parts = {p, 0u, p, p, 0u};
        Buf out = renderQueued(eng, q, in, parts, tl);
        CHECK(allFinite(out), "non-finite output at partition %u", p);
        CHECK(eng.diagnostics().samples == int64_t(N), "sample counter %lld", (long long)eng.diagnostics().samples);
        if (ref.empty()) { ref = std::move(out); continue; }
        const double d = peakDiff(ref, out);
        worst = std::max(worst, d);
        if (d != 0.0) bitIdentical = false;
        CHECK(d <= 1e-12, "partition %u differs by %.3e", p, d);
    }
    std::printf("T06/wiring partitions {1,17,64,127,512,8192}+zero frames, 60 queued events at random offsets: peak diff %.3e (%s)\n",
                worst, bitIdentical ? "bit-identical" : "not bit-identical");
}

// Step event at offset k: visible exactly at k + latency (Zero Latency: latency 0).
void sampleAccurateTiming() {
    std::string e;
    {
        EngineConfig c = baseConfig(1);
        c.quality.zeroLatency = true;
        const Buf in = makeInput(1, 256, 3);
        Engine a, b;
        CHECK(a.prepare(c, e) && b.prepare(c, e), "prepare zl %s", e.c_str());
        CHECK(a.latencySamples() == 0, "zero latency engine declares %d", a.latencySamples());
        TransactionQueue qa, qb;
        const uint32_t k = 37;
        Buf ya = renderQueued(a, qa, in, {128u}, {{k, 1005, norm(1005, 6.0)}});
        Buf yb = renderQueued(b, qb, in, {128u}, {});
        std::size_t first = 256;
        for (std::size_t i = 0; i < 256; ++i) if (ya[0][i] != yb[0][i]) { first = i; break; }
        CHECK(first == k, "Zero Latency: input-gain step at %u first visible at %zu", k, first);
        std::printf("Event timing (Zero Latency, L=0): step at offset %u first visible at sample %zu\n", k, first);
    }
    {
        EngineConfig c = baseConfig(1);
        const Buf in = makeInput(1, 1024, 4);
        Engine a, b, r;
        CHECK(a.prepare(c, e) && b.prepare(c, e) && r.prepare(c, e), "prepare %s", e.c_str());
        const int L = a.latencySamples();
        TransactionQueue qa, qb, qr;
        const uint32_t k = 300;
        Buf ya = renderQueued(a, qa, in, {512u}, {{k, 1005, norm(1005, 6.0)}});        // event at offset 300
        Buf yb = renderQueued(b, qb, in, {300u, 212u, 512u}, {{k, 1005, norm(1005, 6.0)}});  // call split at 300, offset 0
        Buf yr = renderQueued(r, qr, in, {512u}, {});
        std::size_t first = 1024;
        for (std::size_t i = 0; i < 1024; ++i) if (ya[0][i] != yr[0][i]) { first = i; break; }
        CHECK(peakDiff(ya, yb) == 0.0, "event at offset %u differs from call split at %u: %.3e", k, k, peakDiff(ya, yb));
        CHECK(first >= k && first <= k + uint32_t(L), "Studio: first visible %zu outside [k, k+L=%d]", first, int(k) + L);
        std::printf("Event timing (Studio, L=%d): offset-%u event == call split at %u (bit-identical); first output change at %zu "
                    "(linear-phase FIR pre-response %zu samples before k+L)\n", L, k, k, first, std::size_t(k) + std::size_t(L) - first);
    }
}

// T06 at engine level.
void macrosT06() {
    std::string e;
    const Buf in = makeInput(2, 4800, 5);
    // Macro 0 identity vs an engine without macro bindings.
    {
        EngineConfig withMacros = baseConfig(), noMacros = baseConfig();
        noMacros.macroBindings = control::ManifestBindings{};
        Engine a, b;
        CHECK(a.prepare(withMacros, e) && b.prepare(noMacros, e), "prepare %s", e.c_str());
        TransactionQueue qa, qb;
        const std::vector<TimedEvent> zeros = {{0, 1000, 0.0}, {0, 1001, 0.0}, {0, 1002, 0.0}, {0, 1003, 0.0}, {100, 1003, 0.0}};
        Buf ya = renderQueued(a, qa, in, {256u}, zeros);
        Buf yb = renderQueued(b, qb, in, {256u}, {});
        const double d = peakDiff(ya, yb);
        CHECK(d <= 1e-7, "macro 0 vs no-macro engine %.3e", d);
        std::printf("T06 engine: macros at 0 vs no-macro engine peak diff %.3e (limit 1e-7)\n", d);
    }
    // Endpoints through a linear unity fixture.
    {
        const double fs = 48000;
        auto measure = [&](control::MacroValues m, double hz, Engine* keep = nullptr) {
            EngineConfig c = linearConfig();
            c.initialMacros = m;
            Engine local;
            Engine& eng = keep ? *keep : local;
            CHECK(eng.prepare(c, e), "prepare %s", e.c_str());
            TransactionQueue q;
            return renderQueued(eng, q, sine(1, 9600, fs, hz, 0.05), {480u}, {});
        };
        Engine ew, ea;
        const Buf y0 = measure({}, 1000), yw = measure({0, 0, 0, 1}, 1000, &ew);
        const double g = std::pow(10.0, 18.0 / 20.0);
        double rel = 0;
        for (std::size_t i = 4800; i < 9600; ++i) rel = std::max(rel, std::fabs(yw[0][i] - g * y0[0][i]));
        rel /= 0.05;
        const auto dw = ew.diagnostics();
        CHECK(std::fabs(dw.effective.driveDb - 18.0) <= 1e-6 && dw.effective.supportsWarmth, "Warmth=1 driveDb %.9f", dw.effective.driveDb);
        CHECK(rel <= 1e-9, "Warmth=1 output != +18 dB x linear reference (rel %.3e)", rel);
        // Age=1: -6 dB shelf at 6 kHz (creative tilt) measured at 18 kHz against the analytic response.
        const Buf a0 = measure({}, 18000), a1 = measure({1, 0, 0, 0}, 18000, &ea);
        const auto da = ea.diagnostics();
        CHECK(std::fabs(da.effective.shelfGainDb + 6.0) <= 1e-6 && da.effective.supportsAge, "Age=1 shelf %.9f dB", da.effective.shelfGainDb);
        auto rms = [](const std::vector<double>& v) { double s = 0; for (std::size_t i = 4800; i < 9600; ++i) s += v[i] * v[i]; return std::sqrt(s / 4800); };
        const double measuredDb = 20 * std::log10(rms(a1[0]) / rms(a0[0]));
        const auto cf = control::designHighShelf(fs, 6000, -6.0, 0.707);
        const double w = 2 * M_PI * 18000 / fs;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        const double analyticDb = 20 * std::log10(std::abs((cf.b0 + cf.b1 * z1 + cf.b2 * z2) / (1.0 + cf.a1 * z1 + cf.a2 * z2)));
        CHECK(std::fabs(measuredDb - analyticDb) <= 1e-3, "Age=1 18 kHz %.6f dB vs analytic %.6f dB", measuredDb, analyticDb);
        std::printf("T06 engine endpoints: Warmth=1 driveDb %.9f, output vs +18 dB linear rel err %.2e; Age=1 shelf %.6f dB, "
                    "18 kHz measured %.6f dB vs RBJ analytic %.6f dB\n", dw.effective.driveDb, rel, da.effective.shelfGainDb, measuredDb, analyticDb);
        // Sag/Drift: no binding in the synthetic fixture -> unsupported, output identical to 0.
        Engine es;
        const Buf ys = measure({0, 1, 1, 0}, 1000, &es);
        const auto ds = es.diagnostics();
        CHECK(!ds.effective.supportsSag && !ds.effective.supportsDrift, "sag/drift should be unsupported on the synthetic fixture");
        CHECK(peakDiff(ys, y0) == 0.0, "unsupported sag/drift changed audio %.3e", peakDiff(ys, y0));
    }
    // Smoothed endpoint via events, base never written back, unsupported/unknown/other-slot counted.
    {
        EngineConfig c = linearConfig();
        c.base.driveDb = 0.0; c.base.serviceAgeYears = 3.0;
        Engine eng, ref;
        CHECK(eng.prepare(c, e) && ref.prepare(c, e), "prepare %s", e.c_str());
        TransactionQueue q;
        const Buf x = sine(1, 24000, 48000, 1000, 0.05);
        renderQueued(eng, q, x, {512u}, {{0, 1000, 1.0}, {0, 1003, 1.0}, {10, 1018, 0.5}});
        const auto d = eng.diagnostics();
        CHECK(std::fabs(d.macros.age - 1.0) <= 1e-6 && std::fabs(d.macros.warmth - 1.0) <= 1e-6, "smoothed endpoints %.9f %.9f",
              d.macros.age, d.macros.warmth);
        CHECK(d.base.serviceAgeYears == 3.0 && d.base.driveDb == 0.0, "base written back");
        CHECK(std::fabs(d.lifecycle - 0.5) <= 1e-12, "lifecycle %.6f", d.lifecycle);
        CHECK(d.effective.serviceAgeYears >= 60.0 - 1e-3, "Age=1 service age %.3f", d.effective.serviceAgeYears);
        // Unsupported/unknown/other-slot events: counted, audio unchanged.
        Engine u1, u2;
        CHECK(u1.prepare(c, e) && u2.prepare(c, e), "prepare");
        const Buf xi = sine(1, 512, 48000, 700, 0.1);
        Buf o1(1, std::vector<double>(512)), o2 = o1;
        const double* ip[1] = {xi[0].data()};
        double* op1[1] = {o1[0].data()};
        double* op2[1] = {o2[0].data()};
        const TransactionEvent evs[] = {TransactionEvent::param(4242, 0.5, 3), TransactionEvent::param(1028, 0.5, 5),
                                        TransactionEvent::param(1040, 1.0, 9), TransactionEvent::param(1020, 1.0, 11),
                                        TransactionEvent::param(1017, 1.0, 12), TransactionEvent::config(9, 1, 20),
                                        TransactionEvent::noteOff(0, 60, 30)};
        {
            Trap t;
            u1.process(ProcessContext{48000, 0, 0, 512, false}, AudioBuses{1, ip, op1}, std::span<const TransactionEvent>(evs));
            u2.process(ProcessContext{48000, 0, 0, 512, false}, AudioBuses{1, ip, op2}, std::span<const TransactionEvent>());
        }
        const auto du = u1.diagnostics();
        CHECK(du.unknownEvents == 1 && du.unsupportedEvents == 3 && du.otherSlotEvents == 1 && du.configEvents == 1 && du.noteEvents == 1,
              "counters unknown %lld unsupported %lld other %lld config %lld note %lld", (long long)du.unknownEvents,
              (long long)du.unsupportedEvents, (long long)du.otherSlotEvents, (long long)du.configEvents, (long long)du.noteEvents);
        CHECK(o1 == o2, "unsupported events changed audio");
    }
}

// ---------------------------------------------------------------------------------------------
const char* kRcBlueprint = R"({"schemaVersion":1,"id":"wiring.rc","name":"Synthetic RC fixture","modelRegistryVersion":"fixture-v1",
 "nodes":["0","in","out"],"components":[{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":2200}},
 {"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8}}],
 "ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})";

blueprint::PreparedHandle compileRc(uint64_t gen, double fs = 48000, int os = 4) {
    const auto r = blueprint::compileText(kRcBlueprint, blueprint::ModelRegistry::syntheticFixtures(), blueprint::RuntimeConfig{fs, os, 1.0}, gen);
    return r.ok ? r.graph : nullptr;
}

void processBlocks(Engine& eng, const Buf& in, Buf& out, std::size_t& pos, int blocks, uint32_t n) {
    for (int b = 0; b < blocks; ++b) {
        const double* ip[1] = {in[0].data() + pos};
        double* op[1] = {out[0].data() + pos};
        Trap t;
        eng.process(ProcessContext{48000, int64_t(pos), 0, n, false}, AudioBuses{1, ip, op}, std::span<const TransactionEvent>());
        pos += n;
    }
}

double maxStep(const std::vector<double>& y, std::size_t a, std::size_t b) {
    double m = 0;
    for (std::size_t i = std::max<std::size_t>(a, 1); i < b; ++i) m = std::max(m, std::fabs(y[i] - y[i - 1]));
    return m;
}

void graphSwap() {
    std::string e;
    EngineConfig c = baseConfig(1, 48000, 256);
    c.initial.autoGain = false;
    Engine eng, ref;
    CHECK(eng.prepare(c, e) && ref.prepare(c, e), "prepare %s", e.c_str());
    GraphInstaller inst(eng.graphTarget());
    CHECK(eng.attachGraphInstaller(&inst), "attach installer");
    GraphTarget wrong = eng.graphTarget();
    wrong.maxBlock = 128;
    GraphInstaller other(wrong);
    Engine tmp;
    CHECK(tmp.prepare(c, e) && !tmp.attachGraphInstaller(&other), "mismatched installer accepted");

    const std::size_t N = 256 * 40;
    const Buf in = sine(1, N, 48000, 440, 0.3);
    Buf y(1, std::vector<double>(N)), yr(1, std::vector<double>(N));
    std::size_t pos = 0, posR = 0;
    // Failed compile / incompatible graph: no change at all.
    const auto bad = blueprint::compileText("{\"schemaVersion\":1", blueprint::ModelRegistry::syntheticFixtures(), blueprint::RuntimeConfig{48000, 4, 1.0}, 1);
    CHECK(!bad.ok && !bad.graph, "malformed blueprint compiled");
    std::string why;
    CHECK(inst.submit(compileRc(1, 96000), why) == GraphInstaller::Status::Failed, "96 kHz graph accepted");
    CHECK(inst.submit(compileRc(1, 48000, 2), why) == GraphInstaller::Status::Failed, "2x graph accepted");
    processBlocks(eng, in, y, pos, 4, 256);
    processBlocks(ref, in, yr, posR, 4, 256);
    CHECK(peakDiff(y, yr) == 0.0 && eng.diagnostics().graphInstalls == 0, "failed compile changed audio");
    CHECK(inst.stats().live == 0 && inst.stats().failed == 2, "failed builds left processors (%d)", inst.stats().live);
    // Install generation 1: exactly one generation increment, 20 ms linear crossfade.
    CHECK(inst.submit(compileRc(1), why) == GraphInstaller::Status::Published, "submit g1: %s", why.c_str());
    CHECK(inst.submit(compileRc(1), why) == GraphInstaller::Status::Stale, "duplicate generation accepted");
    const std::size_t fadeStart = pos;
    processBlocks(eng, in, y, pos, 1, 256);
    auto d = eng.diagnostics();
    CHECK(d.graphInstalls == 1 && d.graphGeneration == 1 && d.graphFading && inst.acknowledged() == 1,
          "install: installs %lld gen %llu fading %d ack %llu", (long long)d.graphInstalls, (unsigned long long)d.graphGeneration,
          int(d.graphFading), (unsigned long long)inst.acknowledged());
    processBlocks(eng, in, y, pos, 3, 256);   // 1024 samples > 960 fade
    d = eng.diagnostics();
    CHECK(!d.graphFading && d.graphFadeLen == 960 && d.graphInstalls == 1 && d.graphGeneration == 1, "fade end state");
    processBlocks(eng, in, y, pos, 8, 256);
    const double stepOld = maxStep(y[0], 256, fadeStart), stepNew = maxStep(y[0], fadeStart + 1200, pos);
    const double stepFade = maxStep(y[0], fadeStart, fadeStart + 960 + 168);
    const double bound = 1.05 * std::max(stepOld, stepNew) + 2.0 / 960.0;
    CHECK(stepFade <= bound, "crossfade step %.4e > bound %.4e", stepFade, bound);
    std::printf("Graph swap: builtin clipper -> RC gen 1, fade 960 samples (20 ms): max |dy| fade %.4e, before %.4e, after %.4e (bound %.4e)\n",
                stepFade, stepOld, stepNew, bound);
    // Retire only after ack + fade: g2 installs, g1 must stay alive while fading out.
    CHECK(inst.submit(compileRc(2), why) == GraphInstaller::Status::Published, "submit g2");
    processBlocks(eng, in, y, pos, 1, 256);
    CHECK(eng.diagnostics().graphGeneration == 2 && eng.diagnostics().graphFading, "g2 not installed");
    CHECK(inst.service(why) == GraphInstaller::Status::Idle && inst.stats().retired == 0 && inst.stats().live == 2,
          "g1 retired while fading (retired %llu live %d)", (unsigned long long)inst.stats().retired, inst.stats().live);
    // Coalescing: g3 and g4 submitted during the fade; only g4 is ever installed.
    CHECK(inst.submit(compileRc(3), why) == GraphInstaller::Status::Pending, "g3 should wait for a released slot");
    CHECK(inst.submit(compileRc(4), why) == GraphInstaller::Status::Pending && inst.stats().coalesced == 1, "g4 should coalesce g3");
    CHECK(inst.stats().live == 2, "more than two processors allocated (%d)", inst.stats().live);
    processBlocks(eng, in, y, pos, 4, 256);
    CHECK(!eng.diagnostics().graphFading, "g2 fade did not finish");
    CHECK(inst.service(why) == GraphInstaller::Status::Published && inst.stats().retired == 1, "g1 not retired after ack (%llu)",
          (unsigned long long)inst.stats().retired);
    processBlocks(eng, in, y, pos, 6, 256);
    d = eng.diagnostics();
    CHECK(d.graphGeneration == 4 && d.graphInstalls == 3, "coalesced install gen %llu installs %lld", (unsigned long long)d.graphGeneration,
          (long long)d.graphInstalls);
    inst.service(why);
    CHECK(inst.stats().retired == 2 && inst.stats().live == 1, "g2 not retired (retired %llu live %d)", (unsigned long long)inst.stats().retired,
          inst.stats().live);
    CHECK(inst.submit(compileRc(3), why) == GraphInstaller::Status::Stale, "stale generation accepted");
    CHECK(allFinite(y), "non-finite output across graph swaps");
    std::printf("Graph lifecycle: stale rejected, failed builds no-op, g3 coalesced into g4, processors live<=2, retired %llu after ack\n",
                (unsigned long long)inst.stats().retired);
    eng.attachGraphInstaller(nullptr);
}

// ---------------------------------------------------------------------------------------------
Buf renderSlots(SlotEngine& s, const Buf& in, uint32_t block, const std::vector<TimedEvent>& tl, int swapAtBlock = -1) {
    TransactionQueue q;
    const std::size_t N = in[0].size();
    Buf out(in.size(), std::vector<double>(N));
    std::size_t pos = 0, ev = 0;
    int b = 0;
    while (pos < N) {
        const uint32_t f = uint32_t(std::min<std::size_t>(block, N - pos));
        if (b++ == swapAtBlock) s.mixer().swap();
        Trap t;
        while (ev < tl.size() && tl[ev].t < pos + f) { q.push(TransactionEvent::param(tl[ev].hostId, tl[ev].norm, uint32_t(tl[ev].t - pos))); ++ev; }
        const double* ip[2] = {in[0].data() + pos, in.size() > 1 ? in[1].data() + pos : nullptr};
        double* op[2] = {out[0].data() + pos, out.size() > 1 ? out[1].data() + pos : nullptr};
        s.processQueued(ProcessContext{48000, int64_t(pos), 0, f, false}, AudioBuses{int(in.size()), ip, op}, q);
        pos += f;
    }
    return out;
}

void abSlots() {
    std::string e;
    const Buf in = makeInput(2, 9600, 21);
    const std::vector<TimedEvent> tl = {{100, 1003, 0.6}, {900, 1000, 0.8}, {2000, 1007, 0.7}, {4000, 1006, norm(1006, -3)}};
    // Identical slots: unity at any position, bit-identical to one engine.
    {
        SlotEngine s;
        CHECK(s.prepare(baseConfig(), baseConfig(), e), "slot prepare %s", e.c_str());
        CHECK(s.mixer().setPosition(0.37), "setPosition");
        Engine single;
        CHECK(single.prepare(baseConfig(), e), "prepare");
        TransactionQueue q;
        const Buf ys = renderSlots(s, in, 512, tl);
        const Buf y1 = renderQueued(single, q, in, {512u}, tl);
        const double d = peakDiff(ys, y1);
        CHECK(d == 0.0, "identical slots differ from single engine by %.3e", d);
        std::printf("A/B identical slots at x=0.37 vs single Engine: peak diff %.3e (unity)\n", d);
    }
    // Swap invariance with different slots.
    {
        EngineConfig a = baseConfig(), b = baseConfig();
        b.initial.mix = 0.5; b.initial.inputGainDb = -6;
        SlotEngine s1, s2;
        CHECK(s1.prepare(a, b, e) && s2.prepare(a, b, e), "slot prepare %s", e.c_str());
        s1.mixer().setPosition(0.3); s2.mixer().setPosition(0.3);
        const Buf y1 = renderSlots(s1, in, 512, tl), y2 = renderSlots(s2, in, 512, tl, 6);
        const double d = peakDiff(y1, y2);
        CHECK(d <= 1e-12 && s2.diagnostics().swaps == 1, "swap changed output by %.3e", d);
        CHECK(s2.mixer().physicalA() == 1, "swap did not move physical A");
        std::printf("A/B swap at block 6 (x=0.3 -> 0.7 on swapped engines): peak diff %.3e vs no swap\n", d);
        // Copy A->B: identical identities -> copied; different topology -> refused.
        SlotEngine s3;
        CHECK(s3.prepare(a, b, e), "prepare");
        s3.mixer().copyAToB();
        renderSlots(s3, in, 512, {});
        CHECK(s3.diagnostics().copies == 1 && s3.engine(1).diagnostics().output.mix == s3.engine(0).diagnostics().output.mix,
              "copy A->B not applied");
        EngineConfig lin = baseConfig();
        lin.circuit = syntheticDivider(2.0);
        SlotEngine s4;
        CHECK(s4.prepare(a, lin, e), "prepare");
        s4.mixer().copyAToB();
        renderSlots(s4, in, 512, {});
        CHECK(s4.diagnostics().copiesRefused == 1 && s4.diagnostics().copies == 0, "cross-topology copy not refused");
    }
    // Equal latency required.
    {
        EngineConfig zl = baseConfig();
        zl.quality.zeroLatency = true;
        SlotEngine s;
        CHECK(!s.prepare(baseConfig(), zl, e), "unequal latency slots accepted");
    }
}

// ---------------------------------------------------------------------------------------------
void snapshots() {
    std::string e;
    const EngineConfig c = baseConfig();
    const Buf in = makeInput(2, 512 * 10, 31);
    Engine e1;
    CHECK(e1.prepare(c, e), "prepare");
    const state::SnapshotIdentity id = e1.snapshotIdentity();
    CHECK(id.layout.layoutId == Engine::kSnapshotLayoutId && id.layout.version == 1 && id.layout.byteSize == e1.snapshotBytes() &&
          id.layout.byteSize > 0 && id.oversampling == 4 && id.sampleRate == 48000, "layout descriptor");
    state::SnapshotExchange x(id);
    CHECK(e1.attachSnapshotExchange(&x), "attach exchange");
    TransactionQueue q;
    std::vector<TimedEvent> tl = {{10, 1000, 0.7}, {700, 1003, 0.5}, {1500, 1007, 0.6}};
    Buf head(2, std::vector<double>(2048));
    {
        Buf part = {std::vector<double>(in[0].begin(), in[0].begin() + 2048), std::vector<double>(in[1].begin(), in[1].begin() + 2048)};
        head = renderQueued(e1, q, part, {512u}, tl);   // blocks 0..3
    }
    const uint64_t token = x.requestCapture();
    Buf o(2, std::vector<double>(512));
    {
        const double* ip[2] = {in[0].data() + 2048, in[1].data() + 2048};
        double* op[2] = {o[0].data(), o[1].data()};
        Trap t;
        e1.process(ProcessContext{48000, 2048, 0, 0, false}, AudioBuses{2, ip, op}, std::span<const TransactionEvent>());  // zero-frame boundary
    }
    state::SavedSnapshot saved;
    x.readLatest(saved);
    CHECK(saved.valid && saved.header.servedRequest >= token && saved.header.sampleIndex == 2048, "capture not served (valid %d idx %llu)",
          int(saved.valid), (unsigned long long)saved.header.sampleIndex);
    // Continue e1; restore into a fresh identical engine and continue with the same input.
    Buf tail(2, std::vector<double>(in[0].size() - 2048));
    Buf tailIn = {std::vector<double>(in[0].begin() + 2048, in[0].end()), std::vector<double>(in[1].begin() + 2048, in[1].end())};
    TransactionQueue q1, q2;
    const std::vector<TimedEvent> later = {{300, 1002, 0.4}, {900, 1005, norm(1005, 3)}};
    const Buf y1 = renderQueued(e1, q1, tailIn, {512u}, later);
    Engine e2;
    CHECK(e2.prepare(c, e), "prepare");
    std::string reason;
    CHECK(e2.restoreSnapshot(saved, reason) == state::RestoreDecision::Restore, "restore refused: %s", reason.c_str());
    CHECK(e2.diagnostics().samples == 2048, "sample counter not restored");
    const Buf y2 = renderQueued(e2, q2, tailIn, {512u}, later);
    const double d = peakDiff(y1, y2);
    CHECK(d <= 1e-6, "snapshot continuation differs by %.3e", d);
    std::printf("Snapshot: layout 'CDE1' v1 %u bytes (stereo Studio clipper); round-trip continuation peak diff %.3e (limit 1e-6)\n",
                id.layout.byteSize, d);
    // Mismatched layout (different topology) -> ResetWarm, configuration-only, equals a fresh engine.
    EngineConfig rc = baseConfig();
    rc.circuit = syntheticRcLowpass(2200, 1e-8);
    Engine e3, fresh;
    CHECK(e3.prepare(rc, e) && fresh.prepare(rc, e), "prepare");
    CHECK(!e3.attachSnapshotExchange(&x), "mismatched exchange attached");
    {   // dirty e3 first so the warm reset is observable
        TransactionQueue qd;
        renderQueued(e3, qd, tailIn, {512u}, {});
    }
    CHECK(e3.restoreSnapshot(saved, reason) == state::RestoreDecision::ResetWarm && !reason.empty(), "mismatch not ResetWarm");
    const std::string mismatchReason = reason;
    TransactionQueue q3, q4;
    const Buf y3 = renderQueued(e3, q3, tailIn, {512u}, {}), y4 = renderQueued(fresh, q4, tailIn, {512u}, {});
    // e3's sample counter persists (configuration-only); audio equals a freshly prepared engine.
    CHECK(peakDiff(y3, y4) == 0.0, "ResetWarm engine differs from fresh %.3e", peakDiff(y3, y4));
    // Corrupt payload with matching identity -> ResetWarm.
    state::SavedSnapshot bad = saved;
    bad.payload[0] = std::byte(uint8_t(bad.payload[0]) ^ 0xFF);
    Engine e5;
    CHECK(e5.prepare(c, e), "prepare");
    CHECK(e5.restoreSnapshot(bad, reason) == state::RestoreDecision::ResetWarm, "corrupt payload restored");
    state::SavedSnapshot none;
    CHECK(e5.restoreSnapshot(none, reason) == state::RestoreDecision::NoCheckpoint, "empty snapshot");
    std::printf("Snapshot: mismatched topology -> ResetWarm (%s); corrupt payload -> ResetWarm; empty -> NoCheckpoint\n", mismatchReason.c_str());
}

void positiveControl() {
    long a = 0;
    {
        RtTrapScope s;
        void* volatile p = ::operator new(64);   // explicit call: not subject to new-expression elision
        ::operator delete(p);
        a = s.counts().allocations;
    }
    CHECK(a >= 1, "RtTrap positive control failed");
}

}  // namespace

int main() {
    positiveControl();
    partitionInvariance();
    sampleAccurateTiming();
    macrosT06();
    graphSwap();
    abSlots();
    snapshots();
    CHECK(g_rtAllocs == 0 && g_rtFrees == 0, "audio calls allocated %ld / freed %ld", g_rtAllocs, g_rtFrees);
    std::printf("UX-09 RtTrap over every wiring audio call (queue push/drain, events, macros, graph install/fade, A/B, capture): "
                "%ld allocations, %ld deallocations; positive control trapped\n", g_rtAllocs, g_rtFrees);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("runtime_wiring: all checks passed\n");
    return 0;
}
