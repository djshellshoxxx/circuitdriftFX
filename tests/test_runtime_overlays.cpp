// CircuitDrift tests: Engine overlay wiring (resume packet 1, remaining stages) — transient and
// sidechain detectors (CD-F15/F16, T15/T16 identity), tempo clock (CD-F13), seeded creative motion
// and pitch-drift delay (D09), fault overlays (CD-F19), telemetry frames (CD-F17), snapshots/A-B
// with the new state, block-partition invariance with every new stage enabled, RtTrap.
// Every circuit is a labelled SYNTHETIC fixture (fixture-v1); fault bounds are the fixture manifest ranges and every I_budget is a
// test value, not a manifest measurement; nothing here validates hardware (G01 stays open).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include "analysis/Telemetry.h"
#include "core/ParameterRegistry.h"
#include "runtime/Engine.h"
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

long g_rtAllocs = 0, g_rtFrees = 0, g_rtLocks = 0, g_rtIo = 0, g_rtCalls = 0;
struct Trap {
    RtTrapScope s;
    ~Trap() {
        const auto c = s.counts();
        g_rtAllocs += c.allocations; g_rtFrees += c.deallocations; g_rtLocks += c.mutexLocks + c.condWaits; g_rtIo += c.ioCalls;
        ++g_rtCalls;
    }
};

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
    double unit() { return double(u()) / 4294967296.0; }
};

using Buf = std::vector<std::vector<double>>;
constexpr double kFs = 48000;

Buf makeInput(int channels, std::size_t n, uint64_t seed, double amp = 0.4) {
    Buf x(static_cast<std::size_t>(channels), std::vector<double>(n));
    Lcg g{seed};
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::sin(2.0 * M_PI * 440.0 * double(i) / kFs);
        const double kick = (i % 12000) < 400 ? 0.9 * std::exp(-double(i % 12000) / 120.0) : 0.0;   // synthetic kicks
        for (int c = 0; c < channels; ++c) x[std::size_t(c)][i] = amp * (0.6 * s + 0.2 * g.d()) + kick;
    }
    return x;
}

Buf sine(int channels, std::size_t n, double hz, double amp) {
    Buf x(static_cast<std::size_t>(channels), std::vector<double>(n));
    for (std::size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) x[std::size_t(c)][i] = amp * std::sin(2.0 * M_PI * hz * double(i) / kFs);
    return x;
}

double peakDiff(const Buf& a, const Buf& b, std::size_t from = 0) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = from; i < a[c].size(); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}
double peak(const Buf& a) { double m = 0; for (const auto& c : a) for (double v : c) m = std::max(m, std::fabs(v)); return m; }
bool allFinite(const Buf& a) { for (const auto& c : a) for (double v : c) if (!std::isfinite(v)) return false; return true; }
uint64_t checksum(const Buf& a) {
    uint64_t h = 1469598103934665603ull;
    for (const auto& c : a) for (double v : c) {
        unsigned char b[8]; std::memcpy(b, &v, 8);
        for (unsigned char x : b) { h ^= x; h *= 1099511628211ull; }
    }
    return h;
}
double norm(uint32_t id, double plain) { return core::toNormalized(*core::findByHostId(id), plain); }

EngineConfig baseConfig(int channels = 2) {
    EngineConfig c;
    c.sampleRate = kFs; c.maxBlock = 512; c.channels = channels;
    c.quality.mode = QualityMode::Studio;
    c.circuit = syntheticDiodeClipper();
    c.initial.autoGain = true;
    return c;
}

EngineConfig linearConfig(int channels = 1) {
    EngineConfig c = baseConfig(channels);
    c.circuit = syntheticDivider(2.0);
    c.initial.autoGain = false; c.initial.softClipEnabled = false; c.initial.hpfEnabled = false;
    return c;
}

EngineConfig driftConfig(EngineConfig c, double drift) {
    c.macroBindings.hasDrift = true;
    c.overlays.pitchDriftEnabled = true;
    c.initialMacros.drift = drift;
    return c;
}

struct TimedEvent { std::size_t t; uint32_t hostId; double norm; };

struct Transport { bool on = false; double bpm = 120; double ppq0 = 0; uint64_t epoch = 0; };

ProcessContext makeCtx(std::size_t pos, uint32_t f, const Transport& tr) {
    ProcessContext ctx{kFs, int64_t(pos), tr.epoch, f, false};
    if (tr.on) {
        ctx.bpm = tr.bpm; ctx.hasTempo = true; ctx.hasPpq = true; ctx.playing = true;
        ctx.ppqPosition = tr.ppq0 + double(pos) * tr.bpm / (60.0 * kFs);
    }
    return ctx;
}

// Transaction-event render; sidechain may be empty (disconnected).
Buf render(Engine& eng, const Buf& in, const std::vector<uint32_t>& parts, const std::vector<TimedEvent>& tl = {},
           const Buf* sc = nullptr, Transport tr = {}) {
    TransactionQueue q;
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
        const double* sp[2] = {};
        for (int c = 0; c < C; ++c) { ip[c] = in[std::size_t(c)].data() + pos; op[c] = out[std::size_t(c)].data() + pos; }
        AudioBuses b{C, ip, op};
        if (sc) {
            for (std::size_t c = 0; c < sc->size(); ++c) sp[c] = (*sc)[c].data() + pos;
            b.sidechainChannels = int(sc->size()); b.sidechain = sp;
        }
        eng.processQueued(makeCtx(pos, f, tr), b, q);
        pos += f;
    }
    return out;
}

struct TimedParam { std::size_t t; ParamEvent e; };

// ProcessBlock render (fault Apply travels as ParamEvents).
Buf renderBlocks(Engine& eng, const Buf& in, const std::vector<uint32_t>& parts, const std::vector<TimedParam>& tl,
                 std::vector<EngineDiagnostics>* perCall = nullptr) {
    const int C = int(in.size());
    const std::size_t N = in[0].size();
    Buf out(std::size_t(C), std::vector<double>(N, 0.0));
    std::size_t pos = 0, ev = 0, k = 0;
    std::vector<ParamEvent> evs;
    evs.reserve(tl.size());
    while (pos < N) {
        const uint32_t f = uint32_t(std::min<std::size_t>(parts[k++ % parts.size()], N - pos));
        evs.clear();
        while (ev < tl.size() && tl[ev].t < pos + f) { ParamEvent e = tl[ev].e; e.offset = uint32_t(tl[ev].t - pos); evs.push_back(e); ++ev; }
        const double* ip[2] = {};
        double* op[2] = {};
        for (int c = 0; c < C; ++c) { ip[c] = in[std::size_t(c)].data() + pos; op[c] = out[std::size_t(c)].data() + pos; }
        ProcessBlock blk;
        blk.ctx = ProcessContext{kFs, int64_t(pos), 0, f, false};
        blk.buses = AudioBuses{C, ip, op};
        blk.events = evs.data(); blk.eventCount = uint32_t(evs.size());
        {
            Trap trap;
            eng.process(blk);
        }
        if (perCall) perCall->push_back(eng.diagnostics());
        pos += f;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
void positiveControl() {
    long a = 0;
    {
        RtTrapScope s;
        void* volatile p = ::operator new(64);
        ::operator delete(p);
        a = s.counts().allocations;
    }
    CHECK(a >= 1, "RtTrap positive control failed");
}

// CD-F15/F16: detectors produce control scalars only. Synthetic fixtures have no supply reservoir,
// so neither P nor I_ext has an audio consumer: amount/depth > 0 and a live sidechain bus must be
// bit-identical to the detector-less engine, and sidechain audio never reaches the output.
void detectors() {
    std::string e;
    const std::size_t N = 24000;
    const Buf in = makeInput(2, N, 3);
    const Buf scLoud = sine(2, N, 100, 0.125892541179417);   // -18 dBFS-peak-ish sine at 100 Hz
    // Reference: detector-less defaults (amount 0, depth 0, Main, no bus).
    Engine ref;
    CHECK(ref.prepare(baseConfig(), e), "prepare %s", e.c_str());
    const Buf y0 = render(ref, in, {256u});
    // amount 0 / depth 0 identity with a connected bus.
    {
        EngineConfig c = baseConfig();
        c.overlays.sidechainSource = control::SidechainSource::External;
        Engine a;
        CHECK(a.prepare(c, e), "prepare");
        const Buf y = render(a, in, {256u}, {}, &scLoud);
        CHECK(peakDiff(y, y0) == 0.0, "T15/T16 amount 0/depth 0 identity %.3e", peakDiff(y, y0));
    }
    // Active detectors: transient amount 1, External depth 1, I_budget 0.5 A, -18 dBFS RMS sine.
    const Buf scRms = sine(2, N, 1000, 0.125892541179417 * std::sqrt(2.0));
    EngineConfig c = baseConfig();
    c.overlays.transientAmount = 1.0;
    c.overlays.sidechainSource = control::SidechainSource::External;
    c.overlays.sidechainDepth = 1.0;
    c.overlays.sidechainBudgetAmps = 0.5;
    Engine a;
    CHECK(a.prepare(c, e), "prepare");
    double bMax = 0;
    Buf y(2, std::vector<double>(N));
    {
        std::size_t pos = 0;
        TransactionQueue q;
        while (pos < N) {
            const uint32_t f = 240;
            const double* ip[2] = {in[0].data() + pos, in[1].data() + pos};
            double* op[2] = {y[0].data() + pos, y[1].data() + pos};
            const double* sp[2] = {scRms[0].data() + pos, scRms[1].data() + pos};
            AudioBuses b{2, ip, op, 2, sp};
            {
                Trap t;
                a.processQueued(makeCtx(pos, f, {}), b, q);
            }
            bMax = std::max(bMax, a.diagnostics().overlay.transientEvidence);
            pos += f;
        }
    }
    const auto d = a.diagnostics().overlay;
    CHECK(peakDiff(y, y0) == 0.0, "active detectors changed audio by %.3e (no consumer exists)", peakDiff(y, y0));
    CHECK(bMax > 0.9, "synthetic kick: transient evidence max %.3f", bMax);
    CHECK(std::fabs(d.sidechainCurrentA - 0.5) <= 0.01, "I_ext %.4f A vs 0.5 A at E = E_ref", d.sidechainCurrentA);
    CHECK(d.loadCurrentA == d.sidechainCurrentA, "External: I_load = I_phys(0) + I_ext (%.4f vs %.4f)", d.loadCurrentA, d.sidechainCurrentA);
    CHECK(d.sidechainConnected && !d.sidechainDisconnected, "sidechain connection state");
    std::printf("CD-F15/F16 detectors: amount/depth 0 identity peak diff 0 (bit-identical); amount 1 + External depth 1 -> output diff "
                "%.3e; kick b_max %.3f; I_ext %.4f A (budget 0.5 A, E=E_ref)\n", peakDiff(y, y0), bMax, d.sidechainCurrentA);
    // Sidechain only: main input silent, loud sidechain -> output identical to silence through the engine.
    {
        const Buf zero(2, std::vector<double>(N, 0.0));
        Engine s1, s2;
        CHECK(s1.prepare(c, e) && s2.prepare(baseConfig(), e), "prepare");
        const Buf ys = render(s1, zero, {512u}, {}, &scLoud), yz = render(s2, zero, {512u});
        CHECK(peakDiff(ys, yz) == 0.0 && peak(ys) == peak(yz), "sidechain audio reached the output (%.3e)", peakDiff(ys, yz));
        CHECK(s1.diagnostics().overlay.sidechainCurrentA > 0, "sidechain-only: detector idle");
        std::printf("CD-F16 sidechain-only: output peak %.3e (silent-input engine %.3e), diff %.3e\n", peak(ys), peak(yz), peakDiff(ys, yz));
    }
    // Disconnected External bus: zero external current + status flag; Blend == External + main term.
    {
        Engine dsc;
        CHECK(dsc.prepare(c, e), "prepare");
        Buf o(2, std::vector<double>(512));
        const double* ip[2] = {in[0].data(), in[1].data()};
        double* op[2] = {o[0].data(), o[1].data()};
        TransactionQueue q;
        ProcessResult r;
        { Trap t; r = dsc.processQueued(makeCtx(0, 512, {}), AudioBuses{2, ip, op}, q); }
        const auto dd = dsc.diagnostics().overlay;
        CHECK((r.status & status::kSidechainDisconnected) && dd.sidechainDisconnected && dd.sidechainCurrentA == 0.0,
              "disconnected bus: status %u I_ext %.3e", r.status, dd.sidechainCurrentA);
    }
    // Registry IDs drive the detectors (sidechain.source/depth/hpf, transient.preserve).
    {
        Engine h;
        CHECK(h.prepare(baseConfig(), e), "prepare");
        const std::vector<TimedEvent> tl = {{0, 1022, norm(1022, 2)}, {0, 1023, norm(1023, 1.5)}, {0, 1024, norm(1024, 80)},
                                            {0, 1025, norm(1025, 0.5)}};
        const Buf yh = render(h, in, {512u}, tl, &scRms);
        const auto dh = h.diagnostics();
        CHECK(dh.overlay.sidechainSource == 2 && dh.unsupportedEvents == 0, "registry sidechain source %d unsupported %lld",
              dh.overlay.sidechainSource, (long long)dh.unsupportedEvents);
        CHECK(std::fabs(dh.overlay.transientStrength - 0.5 * dh.overlay.transientEvidence) <= 1e-15, "P = amount*b");
        CHECK(peakDiff(yh, y0) == 0.0, "registry detector events changed audio");
    }
}

// CD-F13: tempo clock per sample from ProcessContext transport; fallback visible; seek = new epoch.
void tempo() {
    std::string e;
    const std::size_t N = 48000;
    const Buf in = makeInput(2, N, 5);
    EngineConfig c = baseConfig();
    c.overlays.tempoEnabled = true;
    // Missing transport -> 120 BPM fallback, free-running phase, status bit.
    {
        Engine a;
        CHECK(a.prepare(c, e), "prepare");
        Buf o(2, std::vector<double>(480));
        const double* ip[2] = {in[0].data(), in[1].data()};
        double* op[2] = {o[0].data(), o[1].data()};
        TransactionQueue q;
        ProcessResult r;
        { Trap t; r = a.processQueued(makeCtx(0, 480, {}), AudioBuses{2, ip, op}, q); }
        const auto d = a.diagnostics().overlay;
        CHECK((r.status & status::kTempoFallback) && d.tempoStatus.bpmFallback && d.tempoStatus.ppqFreeRun && d.bpm == 120.0,
              "fallback status %u bpm %.1f", r.status, d.bpm);
        CHECK(std::fabs(d.ppq - 480.0 * 2.0 / kFs) <= 1e-12, "free-run ppq %.12f", d.ppq);
    }
    // Host transport 90 BPM, consistent PPQ: phase follows the host; partition invariant.
    Transport tr{true, 90.0, 3.25, 0};
    double ppqRef = 0, phaseRef = 0;
    Buf yRef;
    for (uint32_t p : {512u, 1u, 77u, 480u}) {
        Engine a;
        CHECK(a.prepare(c, e), "prepare");
        const Buf y = render(a, in, {p}, {}, nullptr, tr);
        const auto d = a.diagnostics().overlay;
        CHECK(!d.tempoStatus.bpmFallback && !d.tempoStatus.ppqFreeRun && d.bpm == 90.0, "host tempo not used");
        const double expect = 3.25 + double(N) * 90.0 / (60.0 * kFs);
        CHECK(std::fabs(d.ppq - expect) <= 1e-9, "ppq %.12f vs %.12f", d.ppq, expect);
        if (yRef.empty()) { yRef = y; ppqRef = d.ppq; phaseRef = d.tempoPhase; continue; }
        CHECK(d.ppq == ppqRef && d.tempoPhase == phaseRef, "tempo partition %u: ppq %.17g vs %.17g", p, d.ppq, ppqRef);
        CHECK(peakDiff(y, yRef) == 0.0, "tempo partition %u audio differs", p);
    }
    std::printf("CD-F13 tempo: fallback 120 BPM + free-run flagged without transport; host 90 BPM ppq %.9f after 1 s, partitions "
                "{512,1,77,480} bit-identical ppq/phase\n", ppqRef);
    // Seek: new epoch with a jumped PPQ clears the phase lock (ppq resyncs) but no audio/thermal state.
    {
        Engine a, b;
        CHECK(a.prepare(c, e) && b.prepare(c, e), "prepare");
        Buf ya(2, std::vector<double>(N)), yb = ya;
        std::size_t pos = 0;
        TransactionQueue qa, qb;
        while (pos < N) {
            const uint32_t f = 480;
            Transport ta = tr, tb = tr;
            if (pos >= 24000) { ta.epoch = 1; ta.ppq0 = 100.0 - 24000.0 * 90.0 / (60.0 * kFs); }   // seek to ppq 100 at the half
            const double* ip[2] = {in[0].data() + pos, in[1].data() + pos};
            double* oa[2] = {ya[0].data() + pos, ya[1].data() + pos};
            double* ob[2] = {yb[0].data() + pos, yb[1].data() + pos};
            {
                Trap t;
                a.processQueued(makeCtx(pos, f, ta), AudioBuses{2, ip, oa}, qa);
                b.processQueued(makeCtx(pos, f, tb), AudioBuses{2, ip, ob}, qb);
            }
            pos += f;
        }
        const auto da = a.diagnostics(), db = b.diagnostics();
        const double expect = 100.0 + 24000.0 * 90.0 / (60.0 * kFs);
        CHECK(da.epochChanges == 1 && da.overlay.tempoResets == 1, "seek counters %lld %lld", (long long)da.epochChanges,
              (long long)da.overlay.tempoResets);
        CHECK(std::fabs(da.overlay.ppq - expect) <= 1e-9, "post-seek ppq %.9f vs %.9f", da.overlay.ppq, expect);
        CHECK(peakDiff(ya, yb) == 0.0, "seek changed audio by %.3e", peakDiff(ya, yb));
        CHECK(a.active()->circuit().temperatureK(0, 0) == b.active()->circuit().temperatureK(0, 0) &&
                  da.circuit.thermalTicks == db.circuit.thermalTicks,
              "seek touched thermal state");
        std::printf("CD-F13 seek (epoch 0->1, ppq -> 100): tempo resets %lld, ppq %.6f; audio and thermal state unchanged (diff %.3e)\n",
                    (long long)da.overlay.tempoResets, da.overlay.ppq, peakDiff(ya, yb));
    }
}

// D09: seeded creative motion + pitch-drift delay on the wet path (Drift binding, not Zero Latency).
void creativeMotion() {
    std::string e;
    const std::size_t N = 48000;
    const Buf x = sine(1, N, 997, 0.3);
    Engine ref;
    CHECK(ref.prepare(linearConfig(), e), "prepare");
    const Buf y0 = render(ref, x, {480u});
    const int L0 = ref.latencySamples();
    // d = 0: exactly the drift-less engine delayed by the 72-sample base delay.
    {
        Engine a;
        CHECK(a.prepare(driftConfig(linearConfig(), 0.0), e), "prepare %s", e.c_str());
        const int L = a.latencySamples();
        CHECK(L == L0 + 72 && a.quality().driftLatency == 72 && a.diagnostics().overlay.driftDelayActive, "drift latency %d vs %d", L, L0);
        const Buf y = render(a, x, {480u});
        double m = 0;
        for (std::size_t i = 72; i < N; ++i) m = std::max(m, std::fabs(y[0][i] - y0[0][i - 72]));
        CHECK(m == 0.0, "d=0 drift engine != reference delayed by 72 (%.3e)", m);
        std::printf("D09 d=0: latency %d = %d + 72 (1.5 ms @48 kHz); output == drift-less engine delayed 72 samples (peak diff %.3e)\n", L, L0, m);
    }
    // d = 1: bounded deviation, slew-limited, seeded, partition invariant.
    Buf yRef;
    double devMax = 0, slewMax = 0;
    for (uint32_t p : {480u, 1u, 33u, 512u, 8192u}) {
        Engine a;
        CHECK(a.prepare(driftConfig(linearConfig(), 1.0), e), "prepare");
        Buf y(1, std::vector<double>(N));
        std::size_t pos = 0;
        TransactionQueue q;
        double prev = a.diagnostics().overlay.driftDelaySamples;
        while (pos < N) {
            const uint32_t f = uint32_t(std::min<std::size_t>(p, N - pos));
            const double* ip[1] = {x[0].data() + pos};
            double* op[1] = {y[0].data() + pos};
            { Trap t; a.processQueued(makeCtx(pos, f, {}), AudioBuses{1, ip, op}, q); }
            const double cur = a.diagnostics().overlay.driftDelaySamples;
            devMax = std::max(devMax, std::fabs(cur - 72.0));
            slewMax = std::max(slewMax, std::fabs(cur - prev) / double(f));
            prev = cur;
            pos += f;
        }
        CHECK(allFinite(y), "drift output not finite");
        if (yRef.empty()) { yRef = y; continue; }
        CHECK(peakDiff(y, yRef) == 0.0, "drift partition %u differs by %.3e", p, peakDiff(y, yRef));
    }
    CHECK(devMax <= 72.0 + 1e-9 && slewMax <= 0.002 + 1e-12, "deviation %.4f samples, slew %.6f samples/sample", devMax, slewMax);
    CHECK(peakDiff(yRef, y0) > 1e-4, "d=1 had no effect");
    // Seeds: same seed identical, different seed differs.
    {
        EngineConfig c2 = driftConfig(linearConfig(), 1.0);
        c2.overlays.motionSeed = 1234;
        Engine a, b;
        CHECK(a.prepare(c2, e) && b.prepare(c2, e), "prepare");
        const Buf ya = render(a, x, {480u}), yb = render(b, x, {480u});
        CHECK(peakDiff(ya, yb) == 0.0 && peakDiff(ya, yRef) > 0.0, "seed repeatability");
    }
    std::printf("D09 d=1: |delay-1.5 ms| max %.3f samples (limit 72 = 1.5 ms), slew max %.6f s/s (limit 0.002); partitions "
                "{480,1,33,512,8192} bit-identical; seed-repeatable\n", devMax, slewMax);
    // Zero Latency: delay disabled, latency and audio identical to a drift-unbound ZL engine.
    {
        EngineConfig zl = linearConfig();
        zl.quality.zeroLatency = true;
        Engine a, b;
        CHECK(a.prepare(driftConfig(zl, 1.0), e) && b.prepare(zl, e), "prepare");
        CHECK(!a.diagnostics().overlay.driftDelayActive && a.latencySamples() == b.latencySamples() && a.latencySamples() == 0,
              "Zero Latency drift delay active (latency %d)", a.latencySamples());
        const Buf ya = render(a, x, {480u}), yb = render(b, x, {480u});
        CHECK(peakDiff(ya, yb) == 0.0, "Zero Latency drift changed audio by %.3e", peakDiff(ya, yb));
    }
    // pitchDrift.enabled at runtime needs re-prepare: counted unsupported, audio unchanged.
    {
        Engine a;
        CHECK(a.prepare(linearConfig(), e), "prepare");
        const Buf ya = render(a, x, {480u}, {{0, 1016, 1.0}});
        CHECK(a.diagnostics().unsupportedEvents == 1 && peakDiff(ya, y0) == 0.0, "pitchDrift.enabled runtime toggle");
    }
}

// CD-F19: fault channels at the electrical rate through the ParamEvent Apply path.
EngineConfig faultConfig() {
    EngineConfig c = baseConfig(1);
    auto& f = c.overlays.faults;
    f[0].used = true; f[0].element = 1;   // fixture.resistor 1 kOhm (bounds derived from netlist + manifest)
    f[0].overlay.type = blueprint::FaultType::CrackedResistor; f[0].overlay.severity = 1.0; f[0].overlay.seed = 42;
    f[1].used = true; f[1].element = 4;   // fixture.capacitor 10 nF: no leakage parameter -> leaky unavailable
    f[1].overlay.type = blueprint::FaultType::ElectrolyticLeaky; f[1].overlay.severity = 1.0;
    f[2].used = true; f[2].element = 2;   // diode: microphonic tube fault has no validated topology
    f[2].overlay.type = blueprint::FaultType::MicrophonicTube;
    return c;
}

void faults() {
    std::string e;
    const std::size_t N = 96000;   // 2 s
    const Buf in = makeInput(1, N, 9);
    auto ev = [](ParamTarget t, double v, uint32_t idx) { ParamEvent p; p.target = t; p.value = v; p.index = idx; return p; };
    const std::vector<TimedParam> tl = {{1000, ev(ParamTarget::FaultEnabled, 1, 0)}, {1000, ev(ParamTarget::FaultEnabled, 1, 1)},
                                        {1000, ev(ParamTarget::FaultEnabled, 1, 2)}, {72000, ev(ParamTarget::FaultResetAll, 1, 0)}};
    Engine none;
    CHECK(none.prepare(baseConfig(1), e), "prepare");
    const Buf y0 = renderBlocks(none, in, {512u}, {});
    Engine a;
    CHECK(a.prepare(faultConfig(), e), "prepare %s", e.c_str());
    const auto d0 = a.diagnostics().overlay;
    CHECK(d0.faults[0].available && !d0.faults[1].available && std::strstr(d0.faults[1].reason, "leakage") && !d0.faults[2].available &&
              std::strstr(d0.faults[2].reason, "tube"),
          "fault availability (%s / %s)", d0.faults[1].reason, d0.faults[2].reason);
    std::vector<EngineDiagnostics> per;
    const Buf y = renderBlocks(a, in, {480u}, tl, &per);
    // During the fault (before reset at 1.5 s): contact transitions bounded, hook active.
    const EngineDiagnostics& mid = per[size_t(71520 / 480) - 1];
    const std::int64_t transitions = mid.overlay.faults[0].transitions;
    CHECK(mid.overlay.faultActive, "cracked-resistor fault not active");
    CHECK(transitions > 0 && transitions <= 20 * 2, "contact transitions %lld in 1.47 s (limit 20/s)", (long long)transitions);
    CHECK(mid.overlay.faultApplied == 1 && mid.overlay.faultRejected == 2, "applied %lld rejected %lld",
          (long long)mid.overlay.faultApplied, (long long)mid.overlay.faultRejected);
    // Recovery: Reset Faults ramps 20 ms, then no stale overlay remains.
    const auto dEnd = a.diagnostics();
    const auto dRef = none.diagnostics();
    CHECK(!dEnd.overlay.faultActive && dEnd.overlay.faultHook.leakFracAdd == 0.0 && dEnd.overlay.faultHook.biasOffsetAdd == 0.0 &&
              dEnd.overlay.faults[0].cleared && dEnd.overlay.faults[1].cleared && !dEnd.overlay.faults[0].overlay.enabled,
          "stale overlay after Reset Faults");
    CHECK(dEnd.effective.leakFrac == dRef.effective.leakFrac && dEnd.effective.biasOffset == dRef.effective.biasOffset &&
              dEnd.overlay.faults[0].out.resistanceOhm == 1e3 && dEnd.overlay.faults[0].applied == 1e3,
          "effective values not back to nominal: leak %.17g vs %.17g, R %.17g", dEnd.effective.leakFrac, dRef.effective.leakFrac,
          dEnd.overlay.faults[0].out.resistanceOhm);
    // The fault reaches the netlist through MnaSolver::updateParameter (details: test_runtime_faults.cpp).
    CHECK(peakDiff(y, y0) > 1e-3 && dEnd.overlay.faultParamRefused == 0, "cracked resistor did not change audio (%.3e) / refused %lld",
          peakDiff(y, y0), (long long)dEnd.overlay.faultParamRefused);
    // Seeded repeatability + partition invariance of the fault state.
    {
        Engine b;
        CHECK(b.prepare(faultConfig(), e), "prepare");
        std::vector<EngineDiagnostics> perB;
        renderBlocks(b, in, {480u}, tl, &perB);
        Engine c;
        CHECK(c.prepare(faultConfig(), e), "prepare");
        std::vector<TimedParam> tlEarly = tl;
        renderBlocks(c, Buf(1, std::vector<double>(in[0].begin(), in[0].begin() + 71520)), {7u, 113u, 512u}, tlEarly);
        const auto dc = c.diagnostics().overlay;
        CHECK(perB[size_t(71520 / 480) - 1].overlay.faults[0].transitions == transitions && dc.faults[0].transitions == transitions &&
                  dc.faults[0].out.resistanceOhm == mid.overlay.faults[0].out.resistanceOhm &&
                  dc.faults[0].out.contactOpen == mid.overlay.faults[0].out.contactOpen,
              "seeded intermittent pattern not repeatable/partition invariant (%lld vs %lld)", (long long)dc.faults[0].transitions,
              (long long)transitions);
        EngineConfig other = faultConfig();
        other.overlays.faults[0].overlay.seed = 43;
        Engine s2;
        CHECK(s2.prepare(other, e), "prepare");
        std::vector<EngineDiagnostics> perS;
        renderBlocks(s2, in, {480u}, tl, &perS);
        bool differs = false;
        for (std::size_t k = 0; k < perS.size() && k < per.size() && !differs; ++k)
            differs = perS[k].overlay.faults[0].out.contactOpen != per[k].overlay.faults[0].out.contactOpen;
        CHECK(differs, "seed 43 reproduced seed 42's contact pattern");
    }
    std::printf("CD-F19 faults: cracked-resistor seed 42 %lld transitions in 1.47 s (<=20/s), audio peak diff %.3e vs fault-free; leaky "
                "unavailable ('%s'); tube unavailable ('%s'); Reset Faults -> no stale overlay, solver R nominal, effective == fault-free "
                "engine; seeded + partition-invariant\n",
                (long long)transitions, peakDiff(y, y0), d0.faults[1].reason, d0.faults[2].reason);
}

// CD-F17: telemetry frames at every 128-sample quantum; reader state never changes audio.
void telemetry() {
    std::string e;
    const std::size_t N = 48000;
    const Buf in = makeInput(2, N, 13);
    Engine plain;
    CHECK(plain.prepare(baseConfig(), e), "prepare");
    const Buf y0 = render(plain, in, {480u});
    // Live reader.
    {
        analysis::TelemetryChannel ch(512);
        analysis::TelemetryWriter w(ch);
        analysis::TelemetryReader r(ch);
        Engine a;
        CHECK(a.prepare(baseConfig(), e), "prepare");
        a.attachTelemetry(&w);
        Buf y(2, std::vector<double>(N));
        std::size_t pos = 0, frames = 0, gaps = 0;
        bool idxOk = true, unitsOk = true;
        int comps = -1;
        TransactionQueue q;
        analysis::DisplayFrame df;
        while (pos < N) {
            const uint32_t f = 480;
            const double* ip[2] = {in[0].data() + pos, in[1].data() + pos};
            double* op[2] = {y[0].data() + pos, y[1].data() + pos};
            { Trap t; a.processQueued(makeCtx(pos, f, {}), AudioBuses{2, ip, op}, q); }
            pos += f;
            while (r.poll(df, double(pos) / kFs)) {
                ++frames;
                gaps += df.missedBefore;
                if (df.raw.sampleIndex % 128 != 0 || df.raw.sampleIndex != int64_t(128 * frames)) idxOk = false;
                comps = int(df.raw.componentCount);
                for (std::uint32_t k = 0; k < df.raw.componentCount; ++k) {
                    const auto& cmp = df.raw.components[k];
                    if (!(cmp.temperatureK > 200 && std::isfinite(cmp.currentA) && std::isfinite(cmp.voltageV) && std::isfinite(cmp.dissipationW)))
                        unitsOk = false;
                }
                if (!(df.raw.validityFlags & analysis::kValidXy) || df.raw.xyCount != 2 || (df.raw.validityFlags & analysis::kValidRail)) unitsOk = false;
            }
        }
        CHECK(frames == N / 128 && gaps == 0 && idxOk, "frames %zu (expected %zu) gaps %zu idx %d", frames, N / 128, gaps, int(idxOk));
        CHECK(comps == 6 && unitsOk, "components %d units %d", comps, int(unitsOk));
        CHECK(peakDiff(y, y0) == 0.0, "telemetry changed audio");
        std::printf("CD-F17 telemetry: %zu frames for %zu samples (1 per 128), 0 gaps, %d branch records (R+2D per channel; K/W/A/V), "
                    "xy in/out volts, rail invalid (no supply in fixture)\n", frames, N, comps);
    }
    // Closed reader, full queue: frames dropped/discarded, audio checksum identical.
    const uint64_t ref = checksum(y0);
    {
        analysis::TelemetryChannel ch(16);
        analysis::TelemetryWriter w(ch);
        Engine a;
        CHECK(a.prepare(baseConfig(), e), "prepare");
        a.attachTelemetry(&w);
        const Buf y = render(a, in, {480u});   // nobody reads: queue fills
        const auto d = a.diagnostics().overlay;
        CHECK(checksum(y) == ref && d.telemetryDropped > 0 && d.telemetryPublished > 0, "full queue: checksum %d dropped %llu",
              int(checksum(y) == ref), (unsigned long long)d.telemetryDropped);
        analysis::TelemetryChannel ch2(64);
        analysis::TelemetryWriter w2(ch2);
        {
            analysis::TelemetryReader r2(ch2);
            r2.close();
        }
        Engine b;
        CHECK(b.prepare(baseConfig(), e), "prepare");
        b.attachTelemetry(&w2);
        const Buf yb = render(b, in, {77u, 512u, 3u});
        const auto db = b.diagnostics().overlay;
        CHECK(checksum(yb) == ref && db.telemetryDiscarded == N / 128 && db.telemetryPublished == 0, "closed reader: checksum %d discarded %llu",
              int(checksum(yb) == ref), (unsigned long long)db.telemetryDiscarded);
        std::printf("CD-F17 telemetry: full queue (cap 16) dropped %llu, closed reader discarded %llu; audio checksum %016llx identical "
                    "to the telemetry-free engine\n", (unsigned long long)d.telemetryDropped, (unsigned long long)db.telemetryDiscarded,
                    (unsigned long long)ref);
    }
}

// Every new stage enabled at once: partitions + zero-frame calls bit-identical.
EngineConfig everythingConfig() {
    EngineConfig c = driftConfig(baseConfig(), 0.7);
    c.overlays.transientAmount = 0.8;
    c.overlays.sidechainSource = control::SidechainSource::Blend;
    c.overlays.sidechainDepth = 1.2;
    c.overlays.sidechainBudgetAmps = 0.3;
    c.overlays.tempoEnabled = true;
    c.overlays.tempoDepth = 0.5;
    c.overlays.faults[0].used = true; c.overlays.faults[0].element = 1;
    c.overlays.faults[0].overlay.type = blueprint::FaultType::CrackedResistor;
    c.overlays.faults[0].overlay.enabled = true; c.overlays.faults[0].overlay.severity = 0.8; c.overlays.faults[0].overlay.seed = 7;
    c.initialMacros.age = 0.3; c.initialMacros.warmth = 0.4;
    return c;
}

void partitionsAllStages() {
    std::string e;
    const std::size_t N = 24000;
    const Buf in = makeInput(2, N, 17);
    const Buf sc = sine(2, N, 60, 0.3);
    const std::vector<TimedEvent> tl = {{100, 1002, 1.0},  {900, 1023, norm(1023, 2.0)}, {3000, 1034, 1.0}, {5000, 1032, norm(1032, 1)},
                                        {7000, 1022, norm(1022, 1)}, {9000, 1025, 0.2}, {12000, 1024, norm(1024, 120)}, {15000, 1033, norm(1033, 2)},
                                        {18000, 1031, 0.0}, {20000, 1003, 0.9}};
    const Transport tr{true, 128.0, 0.5, 0};
    Buf ref;
    for (uint32_t p : {512u, 1u, 17u, 64u, 127u, 8192u}) {
        analysis::TelemetryChannel ch(1024);
        analysis::TelemetryWriter w(ch);
        Engine eng;
        CHECK(eng.prepare(everythingConfig(), e), "prepare %s", e.c_str());
        if (p % 2) eng.attachTelemetry(&w);   // telemetry splitting on odd partitions only
        const Buf y = render(eng, in, {p, 0u, p}, tl, &sc, tr);
        CHECK(allFinite(y), "non-finite output, partition %u", p);
        if (ref.empty()) { ref = y; continue; }
        CHECK(peakDiff(y, ref) == 0.0, "all-stages partition %u differs by %.3e", p, peakDiff(y, ref));
    }
    std::printf("All new stages enabled (detectors, Blend sidechain, tempo+drift depth, pitch drift, cracked-resistor fault, telemetry on "
                "odd partitions, 10 registry events): partitions {512,1,17,64,127,8192}+zero frames bit-identical\n");
}

// Snapshots and A/B copy with the v2 payload.
void snapshotsAndCopy() {
    std::string e;
    const std::size_t N = 512 * 12;
    const Buf in = makeInput(2, N, 23);
    const Buf sc = sine(2, N, 60, 0.3);
    // Without the pitch delay: capture mid-stream, restore into a fresh engine, continue identically.
    EngineConfig c = everythingConfig();
    c.overlays.pitchDriftEnabled = false;
    Engine e1;
    CHECK(e1.prepare(c, e), "prepare");
    CHECK(e1.snapshotIdentity().layout.version == Engine::kSnapshotLayoutVersion && Engine::kSnapshotLayoutVersion == 3, "layout version");
    const Buf head(2, std::vector<double>(in[0].begin(), in[0].begin() + 3072));
    Buf headIn = {std::vector<double>(in[0].begin(), in[0].begin() + 3072), std::vector<double>(in[1].begin(), in[1].begin() + 3072)};
    Buf scHead = {std::vector<double>(sc[0].begin(), sc[0].begin() + 3072), std::vector<double>(sc[1].begin(), sc[1].begin() + 3072)};
    const Transport tr{true, 128.0, 0.5, 0};
    render(e1, headIn, {512u}, {}, &scHead, tr);
    std::vector<std::byte> blob(e1.snapshotBytes());
    std::size_t got = 0;
    { Trap t; got = e1.captureState(blob); }
    CHECK(got == blob.size() && got > 0, "capture %zu", got);
    state::SavedSnapshot saved;
    saved.valid = true;
    saved.header.identity = e1.snapshotIdentity();
    saved.payload = blob;
    Buf tailIn = {std::vector<double>(in[0].begin() + 3072, in[0].end()), std::vector<double>(in[1].begin() + 3072, in[1].end())};
    Buf scTail = {std::vector<double>(sc[0].begin() + 3072, sc[0].end()), std::vector<double>(sc[1].begin() + 3072, sc[1].end())};
    Transport tt = tr;
    tt.ppq0 = 0.5 + 3072.0 * 128.0 / (60.0 * kFs);
    const Buf y1 = render(e1, tailIn, {512u}, {}, &scTail, tt);
    Engine e2;
    CHECK(e2.prepare(c, e), "prepare");
    std::string why;
    const auto dec = e2.restoreSnapshot(saved, why);
    CHECK(dec == state::RestoreDecision::Restore, "restore refused: %s", why.c_str());
    const Buf y2 = render(e2, tailIn, {512u}, {}, &scTail, tt);
    CHECK(peakDiff(y1, y2) == 0.0, "v2 snapshot continuation differs %.3e", peakDiff(y1, y2));
    CHECK(e1.diagnostics().overlay.faults[0].transitions == e2.diagnostics().overlay.faults[0].transitions, "fault state not restored");
    std::printf("Snapshot v3 (detectors, tempo, motion, faults; no pitch delay in this config): %zu bytes, continuation peak diff %.3e\n", got,
                peakDiff(y1, y2));
    // With the pitch delay (layout v3): the payload carries each channel's delay ring and slewed delay,
    // so both A/B copy and payload restore continue exactly and allocation-free. A corrupted delay
    // state (position/delay out of range, non-finite ring sample) is rejected whole.
    {
        const EngineConfig cd = everythingConfig();
        Engine a, b, r;
        CHECK(a.prepare(cd, e) && b.prepare(cd, e) && r.prepare(cd, e), "prepare");
        render(a, headIn, {512u}, {}, &scHead, tr);
        std::vector<std::byte> scratch(a.snapshotBytes());
        bool ok = false;
        { Trap t; ok = b.copyStateFrom(a, scratch); }
        CHECK(ok, "A/B copy with pitch delay");
        std::vector<std::byte> s2(r.snapshotBytes());
        bool loaded = false;
        { Trap t; loaded = a.captureState(s2) == s2.size() && r.loadState(s2); }
        CHECK(loaded, "pitch-drift engine load");
        const Buf ya = render(a, tailIn, {512u}, {}, &scTail, tt), yb = render(b, tailIn, {512u}, {}, &scTail, tt),
                  yr = render(r, tailIn, {512u}, {}, &scTail, tt);
        CHECK(peakDiff(ya, yb) == 0.0, "A/B copy continuation differs %.3e", peakDiff(ya, yb));
        CHECK(peakDiff(ya, yr) == 0.0, "payload restore continuation differs %.3e", peakDiff(ya, yr));
        // Corruption: the last channel's pitch-delay block ends the payload: uint64 prefix (ring+2)
        // followed by [position, current delay, ring...]. Ring length follows PitchDriftDelay::prepare.
        {
            std::size_t ring = 8;
            const std::size_t need = std::size_t(std::ceil(0.008 * cd.sampleRate)) + 4;
            while (ring < need) ring <<= 1;
            const std::size_t stateDoubles = ring + 2;
            const std::size_t blockStart = s2.size() - 8 - stateDoubles * 8;
            std::uint64_t prefix = 0;
            std::memcpy(&prefix, s2.data() + blockStart, sizeof(prefix));
            CHECK(prefix == stateDoubles, "pitch-delay block prefix %llu != %zu", (unsigned long long)prefix, stateDoubles);
            std::vector<std::byte> bad = s2;
            const double nan = std::numeric_limits<double>::quiet_NaN();
            std::memcpy(bad.data() + bad.size() - sizeof(double), &nan, sizeof(double));
            CHECK(!r.loadState(bad), "non-finite ring sample accepted");
            bad = s2;
            const double tooFar = double(ring);   // current delay beyond the ring
            std::memcpy(bad.data() + blockStart + 8 + sizeof(double), &tooFar, sizeof(double));
            CHECK(!r.loadState(bad), "out-of-range delay accepted");
            bad = s2;
            const double negPos = -1.0;           // ring position must be an integer in [0, ring)
            std::memcpy(bad.data() + blockStart + 8, &negPos, sizeof(double));
            CHECK(!r.loadState(bad), "negative ring position accepted");
        }
        // The engine is unchanged by the rejected loads: it still continues exactly.
        const Buf yr2 = render(r, tailIn, {512u}, {}, &scTail, tt), ya2 = render(a, tailIn, {512u}, {}, &scTail, tt);
        CHECK(peakDiff(ya2, yr2) == 0.0, "rejected load changed the engine %.3e", peakDiff(ya2, yr2));
        std::printf("Pitch-drift engine (snapshot v3): A/B copy diff %.3e, payload restore diff %.3e, corrupt delay state rejected, "
                    "no allocation\n", peakDiff(ya, yb), peakDiff(ya, yr));
    }
    // SlotEngine forwards the sidechain to both engines (detector state equal in identical slots).
    {
        SlotEngine s;
        EngineConfig sc1 = baseConfig();
        sc1.overlays.sidechainSource = control::SidechainSource::External;
        sc1.overlays.sidechainDepth = 1; sc1.overlays.sidechainBudgetAmps = 1;
        CHECK(s.prepare(sc1, sc1, e), "slot prepare %s", e.c_str());
        Buf o(2, std::vector<double>(512));
        const double* ip[2] = {in[0].data(), in[1].data()};
        double* op[2] = {o[0].data(), o[1].data()};
        const double* sp[2] = {sc[0].data(), sc[1].data()};
        TransactionQueue q;
        { Trap t; s.processQueued(makeCtx(0, 512, {}), AudioBuses{2, ip, op, 2, sp}, q); }
        CHECK(s.engine(0).diagnostics().overlay.sidechainCurrentA > 0 &&
                  s.engine(0).diagnostics().overlay.sidechainCurrentA == s.engine(1).diagnostics().overlay.sidechainCurrentA,
              "SlotEngine did not forward the sidechain");
    }
}

}  // namespace

int main() {
    positiveControl();
    detectors();
    tempo();
    creativeMotion();
    faults();
    telemetry();
    partitionsAllStages();
    snapshotsAndCopy();
    CHECK(g_rtAllocs == 0 && g_rtFrees == 0, "audio calls allocated %ld / freed %ld", g_rtAllocs, g_rtFrees);
    if (cd::test::kRtTrapLockIoInstrumented) CHECK(g_rtLocks == 0 && g_rtIo == 0, "audio calls locked %ld / I/O %ld", g_rtLocks, g_rtIo);
    std::printf("UX-09 RtTrap over %ld overlay audio calls (detectors, sidechain, tempo/seek, drift delay, faults, telemetry publish, "
                "capture, A/B copy): %ld allocations, %ld deallocations, %ld locks/waits, %ld I/O%s; positive control trapped\n",
                g_rtCalls, g_rtAllocs, g_rtFrees, g_rtLocks, g_rtIo, cd::test::kRtTrapLockIoInstrumented ? "" : " (lock/IO not instrumented)");
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("runtime_overlays: all checks passed\n");
    return 0;
}
