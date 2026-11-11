// CircuitDrift tests: live quality transitions (resume packet 3; T25 part; 01-runtime "Oversampling
// and quality"): off-thread prepared quality, 20 ms linear equal-sum crossfade within one phase
// family at equal padded latency, staged phase-family / Zero Latency / pitch-delay changes behind the
// host deactivate/reactivate latency handshake, no hidden quality change, RtTrap on every audio call.
// The circuit is the labelled SYNTHETIC fixture-v1 diode clipper (G01 open). Owner: engine2 (runtime).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>
#include "runtime/Engine.h"
#include "state/TransactionQueue.h"
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::runtime;
using cd::state::TransactionEvent;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)
long g_rtCount = 0;
constexpr double kFs = 48000;
using Buf = std::vector<std::vector<double>>;

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
};

Buf input(std::size_t n) {
    Buf x(2, std::vector<double>(n, 0.0));
    Lcg g{3};
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::sin(2 * M_PI * 220.0 * double(i) / kFs);
        x[0][i] = 0.5 * s + 0.05 * g.d();
        x[1][i] = 0.4 * s + 0.05 * g.d();
    }
    return x;
}

EngineConfig config(QualityMode m, int manual = 4, dsp::OsFamily fam = dsp::OsFamily::Fir, bool zl = false) {
    EngineConfig c;
    c.sampleRate = kFs; c.maxBlock = 512; c.channels = 2;
    c.quality.mode = m; c.quality.manualFactor = manual; c.quality.family = fam; c.quality.zeroLatency = zl;
    c.circuit = syntheticDiodeClipper();
    c.initial.hpfEnabled = false;   // a fresh HPF state would differ for its 15 Hz memory (not a quality effect)
    return c;
}

std::unique_ptr<PreparedEngine> build(Engine& eng, const EngineConfig& c) {
    std::string e;
    auto p = eng.buildPrepared(c, e);   // non-audio
    CHECK(p != nullptr, "build: %s", e.c_str());
    return p;
}

// One audio callback of `f` frames at absolute position pos, under the RtTrap.
void call(Engine& eng, const Buf& in, Buf& out, std::size_t pos, uint32_t f, std::span<const TransactionEvent> ev = {}) {
    const double* ip[2] = {in[0].data() + pos, in[1].data() + pos};
    double* op[2] = {out[0].data() + pos, out[1].data() + pos};
    cd::test::RtTrapScope trap;
    eng.process(ProcessContext{kFs, int64_t(pos), 0, f, false}, AudioBuses{2, ip, op}, ev);
    g_rtCount += trap.counts().allocations + trap.counts().deallocations;
}

Buf renderPlain(Engine& eng, const Buf& in, uint32_t f) {
    Buf out(2, std::vector<double>(in[0].size(), 0.0));
    for (std::size_t pos = 0; pos < in[0].size(); pos += f) call(eng, in, out, pos, uint32_t(std::min<std::size_t>(f, in[0].size() - pos)));
    return out;
}

double maxStep(const Buf& y, std::size_t a, std::size_t b) {
    double m = 0;
    for (const auto& c : y)
        for (std::size_t i = std::max<std::size_t>(a, 1); i < b; ++i) m = std::max(m, std::fabs(c[i] - c[i - 1]));
    return m;
}
double peakDiff(const Buf& a, const Buf& b, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = from; i < std::min(to, a[c].size()); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}

// ---------------------------------------------------------------------------------------------
void latencyFamilies() {
    std::string e;
    int lat[8] = {};
    const EngineConfig cs[8] = {config(QualityMode::Eco), config(QualityMode::Studio), config(QualityMode::Render),
                                config(QualityMode::Manual, 1), config(QualityMode::Manual, 2), config(QualityMode::Manual, 8),
                                config(QualityMode::Manual, 4, dsp::OsFamily::Iir), config(QualityMode::Studio, 4, dsp::OsFamily::Fir, true)};
    for (int k = 0; k < 8; ++k) {
        Engine eng;
        CHECK(eng.prepare(cs[k], e), "prepare %d: %s", k, e.c_str());
        lat[k] = eng.latencySamples();
    }
    CHECK(lat[0] == lat[1] && lat[1] == lat[2] && lat[2] == lat[3] && lat[3] == lat[4] && lat[4] == lat[5], "FIR family latency differs");
    CHECK(lat[6] != lat[1] && lat[7] == 0, "IIR / Zero Latency should declare a different latency");
    std::printf("Declared latency @48 kHz: FIR family Eco/Studio/Render/1x/2x/8x = %d/%d/%d/%d/%d/%d (padded, constant); IIR 4x %d; "
                "Zero Latency %d\n", lat[0], lat[1], lat[2], lat[3], lat[4], lat[5], lat[6], lat[7]);
}

// Same quality rebuilt: after the pre-roll the incoming path reproduces the old one, so the 20 ms
// equal-sum crossfade is (nearly) transparent and exactly aligned.
void identityCrossfade() {
    const std::size_t N = 256 * 94;
    const Buf in = input(N);
    Engine ref, eng;
    std::string e;
    CHECK(ref.prepare(config(QualityMode::Studio), e) && eng.prepare(config(QualityMode::Studio), e), "prepare");
    const Buf yr = renderPlain(ref, in, 256);
    Buf y(2, std::vector<double>(N, 0.0));
    auto cand = build(eng, config(QualityMode::Studio));
    const std::size_t at = 4096;
    for (std::size_t pos = 0; pos < N; pos += 256) {
        if (pos == at) {
            cd::test::RtTrapScope trap;
            CHECK(eng.requestQuality(cand) == QualityRequest::Crossfading && !cand, "request");
            g_rtCount += trap.counts().allocations + trap.counts().deallocations;
        }
        call(eng, in, y, pos, 256);
    }
    const double d = peakDiff(y, yr);
    const QualityStatus qs = eng.qualityStatus();
    std::unique_ptr<PreparedEngine> old;
    {
        cd::test::RtTrapScope trap;
        CHECK(eng.takeRetired(old) && old, "retired engine handed back");
        g_rtCount += trap.counts().allocations + trap.counts().deallocations;
    }
    old.reset();   // off-audio reclamation
    CHECK(d <= 1e-6 && qs.completed == 1 && !qs.transitioning, "Studio->Studio crossfade not transparent: %.3e", d);
    std::printf("Live crossfade Studio->Studio (rebuilt): pre-roll %d (2L+256) + fade %d samples; output vs no-transition reference peak %.3e "
                "(limit 1e-6)\n", 2 * eng.latencySamples() + Engine::kQualityPrerollMargin, qs.fadeLen, d);
}

// Eco -> Studio -> Render -> Eco: continuity, constant latency, labels, events reach both paths.
void chain() {
    const std::size_t N = 4 * 8192;
    const Buf in = input(N);
    std::string e;
    Engine eng, ref;
    CHECK(eng.prepare(config(QualityMode::Eco), e) && ref.prepare(config(QualityMode::Eco), e), "prepare");
    const Buf yr = renderPlain(ref, in, 128);
    const double baseStep = maxStep(yr, 2000, N);
    Buf y(2, std::vector<double>(N, 0.0));
    const QualityMode seq[3] = {QualityMode::Studio, QualityMode::Render, QualityMode::Eco};
    const char* names[3] = {"Studio 4x FIR", "Render 16x FIR", "Eco 4x FIR"};
    int k = 0, L0 = eng.latencySamples();
    bool latencyConstant = true, labelsOk = qualityLabel(eng.quality()) == "Eco 4x FIR";
    double worstStep = 0, worstDev = 0;
    std::vector<std::size_t> starts;
    for (std::size_t pos = 0; pos < N; pos += 128) {
        if (k < 3 && pos == std::size_t(4096 + k * 8192)) {
            auto cand = build(eng, config(seq[k]));
            const QualityStatus before = eng.qualityStatus();
            cd::test::RtTrapScope trap;
            const QualityRequest r = eng.requestQuality(cand);
            g_rtCount += trap.counts().allocations + trap.counts().deallocations;
            CHECK(r == QualityRequest::Crossfading, "request %d", k);
            const QualityStatus during = eng.qualityStatus();
            // Active label unchanged until the fade completes; incoming labelled separately.
            labelsOk = labelsOk && during.transitioning && qualityLabel(during.incoming) == names[k] &&
                       qualityLabel(during.active) == qualityLabel(before.active);
            starts.push_back(pos);
            ++k;
        }
        // A mix event mid-transition must reach both paths (and the reference).
        TransactionEvent ev = TransactionEvent::param(1007, 0.8, 5);
        const bool sendEv = pos == 4096 + 640;
        call(eng, in, y, pos, 128, sendEv ? std::span<const TransactionEvent>(&ev, 1) : std::span<const TransactionEvent>());
        latencyConstant = latencyConstant && eng.latencySamples() == L0;
        std::unique_ptr<PreparedEngine> old;
        if (eng.takeRetired(old)) old.reset();   // the adapter would hand it to a worker
    }
    (void)yr;
    for (std::size_t s : starts) {
        worstStep = std::max(worstStep, maxStep(y, s, s + 2400));
    }
    // Before the first fade the output is the untouched Eco path.
    worstDev = peakDiff(y, yr, 0, 4096 + std::size_t(L0));   // through the first pre-roll: old path still audible
    const QualityStatus qs = eng.qualityStatus();
    CHECK(qs.completed == 3 && qs.crossfades == 3 && qualityLabel(eng.quality()) == "Eco 4x FIR" && labelsOk, "chain bookkeeping/labels");
    CHECK(latencyConstant, "latency changed within the FIR family");
    CHECK(worstStep <= 1.25 * baseStep, "transition discontinuity: max step %.4e vs steady %.4e", worstStep, baseStep);
    CHECK(worstDev == 0.0, "output changed before the first transition: %.3e", worstDev);
    CHECK(std::fabs(eng.diagnostics().output.mix - 0.8) <= 1e-3, "mix event lost across transitions (%.4f)", eng.diagnostics().output.mix);
    std::printf("Live chain Eco->Studio->Render->Eco (20 ms equal-sum fades): latency constant %d; max |dy| in transitions %.4e vs "
                "steady-state %.4e (limit 1.25x); labels active/incoming separate; mix event applied to both paths\n",
                L0, worstStep, baseStep);
}

// Phase-family change (FIR->IIR), Zero Latency and pitch-delay toggles are staged until reactivation.
void staged() {
    const std::size_t N = 12000;
    const Buf in = input(N);
    std::string e;
    struct Case { const char* name; EngineConfig cfg; } cases[3] = {
        {"FIR->IIR", config(QualityMode::Manual, 4, dsp::OsFamily::Iir)},
        {"Zero Latency on", config(QualityMode::Studio, 4, dsp::OsFamily::Fir, true)},
        {"pitch delay on", config(QualityMode::Studio)}};
    cases[2].cfg.creative.pitchDelay = true;
    for (auto& cs : cases) {
        Engine eng, ref;
        CHECK(eng.prepare(config(QualityMode::Studio), e) && ref.prepare(config(QualityMode::Studio), e), "prepare");
        const int L = eng.latencySamples();
        auto cand = build(eng, cs.cfg);
        const int pendingL = cand->quality().latencySamples;
        QualityRequest r;
        {
            cd::test::RtTrapScope trap;
            r = eng.requestQuality(cand);
            g_rtCount += trap.counts().allocations + trap.counts().deallocations;
        }
        CHECK(r == QualityRequest::Staged && !cand && eng.latencyChangePending() && eng.pendingLatencySamples() == pendingL,
              "%s not staged", cs.name);
        Buf y(2, std::vector<double>(N, 0.0));
        uint32_t st = 0;
        for (std::size_t pos = 0; pos < N; pos += 480) {
            const double* ip[2] = {in[0].data() + pos, in[1].data() + pos};
            double* op[2] = {y[0].data() + pos, y[1].data() + pos};
            cd::test::RtTrapScope trap;
            st |= eng.process(ProcessContext{kFs, int64_t(pos), 0, 480, false}, AudioBuses{2, ip, op}, std::span<const TransactionEvent>()).status;
            g_rtCount += trap.counts().allocations + trap.counts().deallocations;
        }
        const Buf yr = renderPlain(ref, in, 480);
        const QualityStatus qs = eng.qualityStatus();
        CHECK(peakDiff(y, yr) == 0.0 && eng.latencySamples() == L && qualityLabel(qs.active) == "Studio 4x FIR" &&
                  (st & status::kLatencyChangePending) && qs.latencyChangePending,
              "%s: hidden change while pending (diff %.3e, latency %d)", cs.name, peakDiff(y, yr), eng.latencySamples());
        const std::string pendingLabel = qualityLabel(qs.pending);
        // Host deactivate/reactivate: install the pending engine (non-audio).
        CHECK(eng.activatePending(e), "activate %s", e.c_str());
        CHECK(!eng.latencyChangePending() && eng.latencySamples() == pendingL && eng.qualityStatus().activations == 1,
              "%s activation", cs.name);
        std::printf("Staged %-15s: active stays \"Studio 4x FIR\" (latency %d, output bit-identical to no request), pending \"%s\" "
                    "latency %d; after host reactivation latency %d\n", cs.name, L, pendingLabel.c_str(), pendingL, eng.latencySamples());
    }
}

void rejectionsAndRequests() {
    std::string e;
    Engine eng;
    CHECK(eng.prepare(config(QualityMode::Studio), e), "prepare");
    std::unique_ptr<PreparedEngine> none;
    CHECK(eng.requestQuality(none) == QualityRequest::Rejected, "null accepted");
    // Different channel layout.
    EngineConfig mono = config(QualityMode::Eco);
    mono.channels = 1;
    auto bad = build(eng, mono);
    CHECK(eng.requestQuality(bad) == QualityRequest::Rejected && bad, "mono candidate accepted / handle consumed");
    // Stale generation: candidate built before the active engine was re-prepared.
    auto stale = build(eng, config(QualityMode::Eco));
    CHECK(eng.prepare(config(QualityMode::Studio), e), "re-prepare");
    CHECK(eng.requestQuality(stale) == QualityRequest::Rejected && stale, "stale accepted");
    // Second request while a transition runs.
    auto a = build(eng, config(QualityMode::Eco)), b = build(eng, config(QualityMode::Render));
    CHECK(eng.requestQuality(a) == QualityRequest::Crossfading, "first");
    CHECK(eng.requestQuality(b) == QualityRequest::Rejected && b, "second request during a transition accepted");
    // Snapshot capture is refused during a transition.
    std::vector<std::byte> buf(eng.snapshotBytes());
    CHECK(eng.captureState(std::span<std::byte>(buf)) == 0, "capture during transition");
    // Quality host IDs are requests for the adapter, never applied by the engine.
    Engine q1, q2;
    CHECK(q1.prepare(config(QualityMode::Studio), e) && q2.prepare(config(QualityMode::Studio), e), "prepare");
    const Buf in = input(4800);
    Buf y1(2, std::vector<double>(4800)), y2 = y1;
    const TransactionEvent evs[] = {TransactionEvent::param(1012, 1.0, 10), TransactionEvent::param(1014, 1.0, 20),
                                    TransactionEvent::param(1015, 1.0, 30), TransactionEvent::param(1016, 1.0, 40)};
    for (std::size_t pos = 0; pos < 4800; pos += 480) {
        call(q1, in, y1, pos, 480, pos == 0 ? std::span<const TransactionEvent>(evs) : std::span<const TransactionEvent>());
        call(q2, in, y2, pos, 480);
    }
    const QualityStatus s1 = q1.qualityStatus();
    CHECK(y1 == y2 && s1.qualityRequests == 3 && s1.pitchDelayRequests == 1 && qualityLabel(q1.quality()) == "Studio 4x FIR" &&
              q1.diagnostics().unsupportedEvents == 0,
          "quality host IDs changed audio or were not counted");
    std::printf("Rejections: null, layout mismatch, stale generation, request during a transition (handles left intact); capture "
                "refused mid-transition; quality IDs 1012/1014/1015 + pitchDrift 1016 counted as requests (3+1), audio unchanged\n");
}

void rtTrapControl() {
    cd::test::RtTrapScope trap;
    void* p = ::operator new(16);
    ::operator delete(p);
    CHECK(trap.counts().allocations == 1 && trap.counts().deallocations == 1, "RtTrap positive control not trapped");
}

}  // namespace

int main() {
    latencyFamilies();
    identityCrossfade();
    chain();
    staged();
    rejectionsAndRequests();
    rtTrapControl();
    CHECK(g_rtCount == 0, "audio-path allocations/deallocations: %ld", g_rtCount);
    std::printf("UX-09 RtTrap over every transition audio call (request, process during pre-roll/fade, takeRetired): %ld "
                "allocations+deallocations; positive control trapped\n", g_rtCount);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("runtime_quality: all checks passed\n");
    return 0;
}
