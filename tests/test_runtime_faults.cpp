// CircuitDrift tests: CD-F19 fault overlays reaching the built-in circuit through
// MnaSolver::updateParameter (CircuitRuntime parameter tracks). Every circuit is a labelled
// SYNTHETIC fixture (fixture-v1); fault bounds are the fixture manifest ranges (synthetic
// placeholders, no declared open/short equivalents). Faults are artistic bounded simulations:
// nothing here validates a failure model or hardware (G01 stays open).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include "runtime/Engine.h"
#include "state/SnapshotExchange.h"
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::runtime;
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

using Buf = std::vector<std::vector<double>>;
constexpr double kFs = 48000;

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
};

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

Buf slice(const Buf& x, std::size_t a, std::size_t b) {
    Buf y;
    for (const auto& c : x) y.emplace_back(c.begin() + std::ptrdiff_t(a), c.begin() + std::ptrdiff_t(b));
    return y;
}
double peakDiff(const Buf& a, const Buf& b, std::size_t from = 0) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = from; i < a[c].size(); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}
double peak(const Buf& a) { double m = 0; for (const auto& c : a) for (double v : c) m = std::max(m, std::fabs(v)); return m; }
bool allFinite(const Buf& a) { for (const auto& c : a) for (double v : c) if (!std::isfinite(v)) return false; return true; }
bool same(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

struct TimedParam { std::size_t t; ParamEvent e; };
ParamEvent ev(ParamTarget t, double v, uint32_t idx) { ParamEvent p; p.target = t; p.value = v; p.index = idx; return p; }

struct RenderLog {
    std::vector<EngineDiagnostics> perCall;
    uint32_t statusOr = 0;
};

// ProcessBlock render; parts cycle (0 = zero-frame call). Events are delivered at their offsets.
Buf render(Engine& eng, const Buf& in, const std::vector<uint32_t>& parts, const std::vector<TimedParam>& tl = {},
           RenderLog* log = nullptr, std::size_t t0 = 0) {
    const int C = int(in.size());
    const std::size_t N = in[0].size();
    Buf out(std::size_t(C), std::vector<double>(N, 0.0));
    std::size_t pos = 0, evi = 0, k = 0;
    std::vector<ParamEvent> evs;
    evs.reserve(tl.size());
    while (pos < N) {
        const uint32_t f = uint32_t(std::min<std::size_t>(parts[k++ % parts.size()], N - pos));
        evs.clear();
        while (evi < tl.size() && tl[evi].t < pos + f) { ParamEvent e = tl[evi].e; e.offset = uint32_t(tl[evi].t - pos); evs.push_back(e); ++evi; }
        const double* ip[2] = {};
        double* op[2] = {};
        for (int c = 0; c < C; ++c) { ip[c] = in[std::size_t(c)].data() + pos; op[c] = out[std::size_t(c)].data() + pos; }
        ProcessBlock blk;
        blk.ctx = ProcessContext{kFs, int64_t(t0 + pos), 0, f, false};
        blk.buses = AudioBuses{C, ip, op};
        blk.events = evs.data(); blk.eventCount = uint32_t(evs.size());
        ProcessResult r;
        {
            Trap trap;
            r = eng.process(blk);
        }
        if (log) { log->statusOr |= r.status; if (f) log->perCall.push_back(eng.diagnostics()); }
        pos += f;
    }
    return out;
}

EngineConfig base(int channels, CircuitConfig circuit) {
    EngineConfig c;
    c.sampleRate = kFs; c.maxBlock = 512; c.channels = channels;
    c.quality.mode = QualityMode::Studio;
    c.circuit = std::move(circuit);
    c.initial.autoGain = true;
    return c;
}

// Clipper (elements: 0 source, 1 fixture.resistor 1 kOhm series, 2/3 diodes, 4 fixture.capacitor 10 nF).
EngineConfig crackedConfig(double severity, bool enabled, uint64_t seed = 42, int channels = 2) {
    EngineConfig c = base(channels, syntheticDiodeClipper());
    auto& f = c.overlays.faults[0];
    f.used = true; f.element = 1;
    f.overlay.type = blueprint::FaultType::CrackedResistor;
    f.overlay.severity = severity; f.overlay.enabled = enabled; f.overlay.seed = seed;
    return c;
}

// RC lowpass (elements: 0 source, 1 fixture.resistor, 2 fixture.capacitor).
EngineConfig openCapConfig(double severity, bool enabled) {
    EngineConfig c = base(1, syntheticRcLowpass(1000.0, 1e-6));
    auto& f = c.overlays.faults[0];
    f.used = true; f.element = 2;
    f.overlay.type = blueprint::FaultType::ElectrolyticOpen;
    f.overlay.severity = severity; f.overlay.enabled = enabled;
    return c;
}

bool noContainment(const Engine& e, const RenderLog& log) {
    const auto d = e.diagnostics();
    return !(log.statusOr & status::kContainment) && !(log.statusOr & status::kCircuitFallback) && d.circuit.failedSubsteps == 0 &&
           !d.circuit.fallbackLatched && d.output.nonFiniteOutputs == 0 && d.output.clampedOutputs == 0;
}

// Prepare-time bounds and availability.
void availability() {
    std::string e;
    EngineConfig c = base(1, syntheticDiodeClipper());
    auto& f = c.overlays.faults;
    f[0].used = true; f[0].element = 1; f[0].overlay.type = blueprint::FaultType::CrackedResistor;
    f[0].bounds.nominalR = 5; f[0].bounds.openR = 7;   // ignored: derived from the netlist + manifest
    f[1].used = true; f[1].element = 4; f[1].overlay.type = blueprint::FaultType::ElectrolyticOpen;
    f[2].used = true; f[2].element = 4; f[2].overlay.type = blueprint::FaultType::ElectrolyticLeaky;
    f[3].used = true; f[3].element = 2; f[3].overlay.type = blueprint::FaultType::MicrophonicTube;
    Engine a;
    CHECK(a.prepare(c, e), "prepare %s", e.c_str());
    const auto d = a.diagnostics().overlay;
    CHECK(d.faults[0].available && d.faults[0].parameter == 0 && d.faults[0].nominal == 1000.0 && d.faults[0].applied == 1000.0 &&
              d.faults[0].out.resistanceOhm == 0.0 /* not ticked yet */, "cracked resistor bounds");
    CHECK(d.faults[1].available && d.faults[1].parameter == 0 && d.faults[1].nominal == 1e-8 && std::strstr(d.faults[1].partial, "ESR"),
          "electrolytic open: available with ESR half unavailable ('%s')", d.faults[1].partial);
    CHECK(!d.faults[2].available && std::strstr(d.faults[2].reason, "leakage"), "leaky reason '%s'", d.faults[2].reason);
    CHECK(!d.faults[3].available && std::strstr(d.faults[3].reason, "tube"), "tube reason '%s'", d.faults[3].reason);
    CHECK(std::strlen(d.faultPathReason) == 0, "built-in path reports a fault path reason");
    // Bounds come from the manifest: tick once and read the reported placeholder limits through the outputs.
    std::vector<TimedParam> tl = {{0, ev(ParamTarget::FaultSeverity, 1, 0)}, {0, ev(ParamTarget::FaultEnabled, 1, 0)},
                                  {0, ev(ParamTarget::FaultSeverity, 1, 1)}, {0, ev(ParamTarget::FaultEnabled, 1, 1)}};
    RenderLog log;
    render(a, Buf(1, std::vector<double>(4800, 0.0)), {480u}, tl, &log);
    const auto d2 = a.diagnostics().overlay;
    CHECK(d2.faults[1].out.capacitanceF == 1e-15 && d2.faults[1].applied == 1e-15, "open C reached the manifest minimum %.3e / %.3e",
          d2.faults[1].out.capacitanceF, d2.faults[1].applied);
    CHECK(d2.faultParamRefused == 0, "refused %lld", (long long)d2.faultParamRefused);
    std::printf("CD-F19 availability: cracked R (nominal 1000 ohm, open placeholder = manifest max 1e9), electrolytic open (C nominal 1e-8 "
                "-> manifest min 1e-15 F; '%s'), leaky unavailable ('%s'), tube unavailable ('%s')\n",
                d.faults[1].partial, d.faults[2].reason, d.faults[3].reason);
}

// (a) cracked resistor severity 1 + (c) severity 0 / disabled bit-identical + (h) refusals 0.
void cracked() {
    std::string e;
    const std::size_t N = 96000;   // 2 s
    const Buf in = makeInput(2, N, 9);
    Engine none;
    CHECK(none.prepare(base(2, syntheticDiodeClipper()), e), "prepare");
    const Buf y0 = render(none, in, {512u});
    Engine a;
    CHECK(a.prepare(crackedConfig(1.0, true), e), "prepare %s", e.c_str());
    RenderLog log;
    const Buf y = render(a, in, {480u}, {}, &log);
    const auto d = a.diagnostics().overlay;
    const double diff = peakDiff(y, y0);
    const double tps = double(d.faults[0].transitions) / (double(N) / kFs);
    CHECK(diff > 1e-3, "cracked resistor did not change the output (%.3e)", diff);
    CHECK(allFinite(y) && peak(y) <= 8.0, "output not finite/bounded (peak %.3f)", peak(y));
    CHECK(noContainment(a, log), "containment engaged");
    CHECK(d.faults[0].transitions > 0 && tps <= 20.0, "contact transitions %lld (%.2f/s)", (long long)d.faults[0].transitions, tps);
    CHECK(d.faultParamRefused == 0 && d.faultParamUpdates > 0, "updates %lld refused %lld", (long long)d.faultParamUpdates,
          (long long)d.faultParamRefused);
    // Solver follows the fault output every call; contact open -> open-equivalent (manifest max).
    bool follows = true, sawOpen = false;
    double maxR = 0;
    for (const auto& pc : log.perCall) {
        const auto& f = pc.overlay.faults[0];
        follows = follows && same(f.applied, std::clamp(f.out.resistanceOhm, 1e-3, 1e9));
        maxR = std::max(maxR, f.applied);
        sawOpen = sawOpen || (f.out.contactOpen && f.applied == 1e9);
    }
    CHECK(follows, "solver resistance != fault output");
    CHECK(sawOpen && maxR == 1e9, "contact open never reached the open-equivalent (max %.3e)", maxR);
    // Seeded repeatability (different partition too) and seed dependence.
    Engine b;
    CHECK(b.prepare(crackedConfig(1.0, true), e), "prepare");
    const Buf yb = render(b, in, {7u, 113u, 0u, 512u});
    CHECK(peakDiff(y, yb) == 0.0 && b.diagnostics().overlay.faults[0].transitions == d.faults[0].transitions, "not seeded-repeatable");
    Engine s2;
    CHECK(s2.prepare(crackedConfig(1.0, true, 43), e), "prepare");
    const Buf ys = render(s2, in, {480u});
    CHECK(peakDiff(ys, y) > 0.0, "seed 43 reproduced seed 42");
    // (c) severity 0 enabled, and disabled severity 1: bit-identical to the fault-free engine.
    double d0 = 0, d1 = 0;
    {
        Engine z; CHECK(z.prepare(crackedConfig(0.0, true), e), "prepare");
        d0 = peakDiff(render(z, in, {480u}), y0);
        Engine o; CHECK(o.prepare(crackedConfig(1.0, false), e), "prepare");
        d1 = peakDiff(render(o, in, {480u}), y0);
        CHECK(d0 == 0.0 && d1 == 0.0, "severity 0 / disabled not bit-identical (%.3e, %.3e)", d0, d1);
        CHECK(z.diagnostics().overlay.faultParamUpdates == 0 && o.diagnostics().overlay.faultParamUpdates == 0,
              "neutral fault pushed parameter updates");
    }
    std::printf("CD-F19 cracked resistor (clipper series R, sev 1, seed 42): peak diff vs fault-free %.4f, peak %.4f, %lld transitions in "
                "2 s (%.2f/s <= 20), %lld updates / %lld refused, no containment, seeded-repeatable; sev 0 diff %.1e, disabled diff %.1e\n",
                diff, peak(y), (long long)d.faults[0].transitions, tps, (long long)d.faultParamUpdates, (long long)d.faultParamRefused, d0, d1);
}

// (b) electrolytic open on the RC fixture: solver C follows the 10 ms log ramp exactly.
void electrolyticOpen() {
    std::string e;
    const std::size_t N = 4800;
    const Buf in = makeInput(1, N, 3);
    Engine a;
    CHECK(a.prepare(openCapConfig(1.0, false), e), "prepare %s", e.c_str());
    const std::vector<TimedParam> tl = {{480, ev(ParamTarget::FaultEnabled, 1, 0)}};
    RenderLog log;
    const Buf y = render(a, in, {1u}, tl, &log);   // one host sample per call: every OS-th substep observed
    bool follows = true, monotone = true;
    double prev = 1e-6;
    std::size_t reached = 0;
    for (std::size_t i = 0; i < log.perCall.size(); ++i) {
        const auto& f = log.perCall[i].overlay.faults[0];
        follows = follows && same(f.applied, f.out.capacitanceF);
        monotone = monotone && f.applied <= prev;
        prev = f.applied;
        if (!reached && f.applied == 1e-15) reached = i;
    }
    const auto d = a.diagnostics().overlay;
    const double rampMs = double(reached - 480 + 1) * 1000.0 / kFs;
    CHECK(follows, "solver capacitance != FaultOutputs.capacitanceF");
    CHECK(monotone && reached > 480 && std::fabs(rampMs - 10.0) <= 0.05, "ramp to minimum: %.3f ms", rampMs);
    CHECK(a.diagnostics().circuit.failedSubsteps == 0 && allFinite(y) && peak(y) <= 8.0, "RC fault output bounded");
    CHECK(d.faultParamRefused == 0, "refused %lld", (long long)d.faultParamRefused);
    // Mid-ramp: log interpolation (geometric mean of the endpoints at half time).
    const double mid = log.perCall[480 + 240 - 1].overlay.faults[0].applied;
    std::printf("CD-F19 electrolytic open (RC 1k/1uF, sev 1): solver C == FaultOutputs.capacitanceF at every call, 1e-6 -> 1e-15 F "
                "(manifest min) in %.3f ms, mid-ramp %.3e F (log), %lld refused\n",
                rampMs, mid, (long long)d.faultParamRefused);
}

// (d) Reset Faults: 20 ms ramp, parameter back to nominal bit-exactly, output converges.
void resetFaults() {
    std::string e;
    const std::size_t N = 72000, R = 48000;
    const Buf in = makeInput(2, N, 5);
    // Auto-gain and the 15 Hz output HPF off: the loudness-match gain and the HPF's recursive state
    // are slow output-stage states that legitimately remember the faulted output (measured with them
    // on: 5.2e-3 / 2.4e-5 residual 100 ms after reset). They are not fault overlays (Reset Faults
    // touches overlays only), so the circuit-level convergence is measured without them.
    EngineConfig ref = base(2, syntheticDiodeClipper()), cfg = crackedConfig(1.0, true);
    ref.initial.autoGain = false; cfg.initial.autoGain = false;
    ref.initial.hpfEnabled = false; cfg.initial.hpfEnabled = false;
    Engine none;
    CHECK(none.prepare(ref, e), "prepare");
    const Buf y0 = render(none, in, {512u});
    Engine a;
    CHECK(a.prepare(cfg, e), "prepare");
    RenderLog log;
    const Buf y = render(a, in, {480u}, {{R, ev(ParamTarget::FaultResetAll, 1, 0)}}, &log);
    const auto d = a.diagnostics().overlay;
    std::size_t at = 0;   // first call (480 samples) after which the parameter is nominal again
    for (std::size_t i = R / 480; i < log.perCall.size() && !at; ++i)
        if (same(log.perCall[i].overlay.faults[0].applied, 1000.0)) at = i;
    const double rampMs = double((at + 1) * 480 - R) * 1000.0 / kFs;
    CHECK(same(d.faults[0].applied, 1000.0) && same(d.faults[0].applied, d.faults[0].nominal) && d.faults[0].cleared &&
              !d.faults[0].overlay.enabled, "parameter not nominal after Reset: %.17g", d.faults[0].applied);
    CHECK(at > 0 && rampMs <= 20.0 + 10.0, "reset ramp %.1f ms (20 ms ramp, 480-sample call granularity)", rampMs);
    const double before = peakDiff(slice(y, R - 4800, R), slice(y0, R - 4800, R));
    const std::size_t settle = R + 4800;   // 20 ms ramp + 80 ms settling
    const double after = peakDiff(slice(y, settle, N), slice(y0, settle, N));
    CHECK(before > 1e-3 && after <= 1e-9, "post-reset convergence: before %.3e, after settling %.3e", before, after);
    CHECK(d.faultParamRefused == 0, "refused");
    std::printf("CD-F19 Reset Faults: solver R back to 1000 ohm bit-exactly after %.1f ms (call granularity 10 ms); output vs fault-free "
                "%.3e before reset, %.3e over [%.1f s, %.1f s] after settling\n",
                rampMs, before, after, double(settle) / kFs, double(N) / kFs);
}

// (e) partitions with faults active (both types, events mid-stream) bit-identical incl. zero-frame calls.
void partitions() {
    std::string e;
    const std::size_t N = 24000;
    const Buf in = makeInput(2, N, 17);
    EngineConfig c = crackedConfig(0.7, true, 7);
    c.overlays.faults[1].used = true; c.overlays.faults[1].element = 4;
    c.overlays.faults[1].overlay.type = blueprint::FaultType::ElectrolyticOpen;
    c.overlays.faults[1].overlay.severity = 0.6;
    const std::vector<TimedParam> tl = {{3000, ev(ParamTarget::FaultEnabled, 1, 1)}, {9000, ev(ParamTarget::FaultSeverity, 1, 0)},
                                        {15000, ev(ParamTarget::FaultResetAll, 1, 0)}, {18000, ev(ParamTarget::FaultEnabled, 1, 0)},
                                        {18000, ev(ParamTarget::FaultSeverity, 0.5, 0)}};
    Buf ref;
    int checked = 0;
    for (uint32_t p : {512u, 1u, 17u, 64u, 127u, 8192u}) {
        Engine eng;
        CHECK(eng.prepare(c, e), "prepare %s", e.c_str());
        const Buf y = render(eng, in, {p, 0u, p}, tl);
        CHECK(allFinite(y) && eng.diagnostics().overlay.faultParamRefused == 0, "partition %u", p);
        if (ref.empty()) { ref = y; continue; }
        CHECK(peakDiff(y, ref) == 0.0, "partition %u differs by %.3e", p, peakDiff(y, ref));
        ++checked;
    }
    std::printf("CD-F19 partitions {512,1,17,64,127,8192} + zero-frame calls with cracked R + electrolytic open active: %d/5 bit-identical\n",
                checked);
}

// (g) snapshot continuation with active faults: payload restore and A/B copy exact.
void snapshot() {
    std::string e;
    const std::size_t N = 512 * 24, H = 512 * 9 + 37;
    const Buf in = makeInput(2, N, 23);
    EngineConfig c = crackedConfig(1.0, true, 11);
    c.overlays.faults[1].used = true; c.overlays.faults[1].element = 4;
    c.overlays.faults[1].overlay.type = blueprint::FaultType::ElectrolyticOpen;
    c.overlays.faults[1].overlay.severity = 1.0;
    // Electrolytic open applied just before the capture: its 10 ms ramp is mid-flight at the cut.
    const std::vector<TimedParam> headTl = {{H - 200, ev(ParamTarget::FaultEnabled, 1, 1)}};
    Engine e1, e2, e3;
    CHECK(e1.prepare(c, e) && e2.prepare(c, e) && e3.prepare(c, e), "prepare");
    CHECK(Engine::kSnapshotLayoutVersion == 3, "layout version changed");
    render(e1, slice(in, 0, H), {512u}, headTl);
    std::vector<std::byte> blob(e1.snapshotBytes()), scratch(e1.snapshotBytes());
    std::size_t got = 0;
    bool copied = false;
    { Trap t; got = e1.captureState(blob); }
    { Trap t; copied = e3.copyStateFrom(e1, scratch); }
    CHECK(got == blob.size() && copied, "capture/copy");
    state::SavedSnapshot saved;
    saved.valid = true;
    saved.header.identity = e1.snapshotIdentity();
    saved.payload = blob;
    std::string why;
    CHECK(e2.restoreSnapshot(saved, why) == state::RestoreDecision::Restore, "restore refused: %s", why.c_str());
    const auto dMid = e1.diagnostics().overlay;
    const Buf tail = slice(in, H, N);
    const Buf y1 = render(e1, tail, {512u}, {}, nullptr, H);
    const Buf y2 = render(e2, tail, {512u}, {}, nullptr, H);
    const Buf y3 = render(e3, tail, {512u}, {}, nullptr, H);
    const double dr = peakDiff(y1, y2), dc = peakDiff(y1, y3);
    CHECK(dMid.faults[1].applied < 1e-8 && dMid.faults[1].applied > 1e-15, "capture not mid-ramp (C %.3e)", dMid.faults[1].applied);
    CHECK(dr == 0.0 && dc == 0.0, "snapshot continuation differs: restore %.3e, A/B copy %.3e", dr, dc);
    CHECK(same(e1.diagnostics().overlay.faults[0].applied, e2.diagnostics().overlay.faults[0].applied) &&
              same(e1.diagnostics().overlay.faults[1].applied, e2.diagnostics().overlay.faults[1].applied), "applied values differ");
    std::printf("CD-F19 snapshot (layout v3 unchanged, %zu bytes) with cracked R + mid-ramp electrolytic open (C %.3e F at the cut): "
                "continuation diff restore %.1e, A/B copy %.1e\n", got, dMid.faults[1].applied, dr, dc);
}

}  // namespace

int main() {
    {
        long a = 0;
        { RtTrapScope s; void* volatile p = ::operator new(64); ::operator delete(p); a = s.counts().allocations; }
        CHECK(a >= 1, "RtTrap positive control failed");
    }
    availability();
    cracked();
    electrolyticOpen();
    resetFaults();
    partitions();
    snapshot();
    CHECK(g_rtAllocs == 0 && g_rtFrees == 0, "audio calls allocated %ld / freed %ld", g_rtAllocs, g_rtFrees);
    if (cd::test::kRtTrapLockIoInstrumented) CHECK(g_rtLocks == 0 && g_rtIo == 0, "audio calls locked %ld / I/O %ld", g_rtLocks, g_rtIo);
    std::printf("UX-09 RtTrap over %ld fault-active audio calls (incl. capture, A/B copy): %ld allocations, %ld deallocations, %ld "
                "locks/waits, %ld I/O%s\n", g_rtCalls, g_rtAllocs, g_rtFrees, g_rtLocks, g_rtIo,
                cd::test::kRtTrapLockIoInstrumented ? "" : " (lock/IO not instrumented)");
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("runtime_faults: all checks passed\n");
    return 0;
}
