// Regression tests for verified review findings (C1, H1, M1-M3, L1-L7). Each check names its finding.
// All data is SYNTHETIC; nothing here validates physical behaviour (G01 stays open).
// Multi-threaded sections (M2) are meant to be run under -fsanitize=thread as well.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "blueprint/Compiler.h"
#include "blueprint/Faults.h"
#include "community/CatalogClient.h"
#include "community/MockCatalogService.h"
#include "control/Macros.h"
#include "control/PitchDetector.h"
#include "core/Json.h"
#include "dsp/MnaSolver.h"
#include "state/History.h"
#include "state/StateDocument.h"
#include "state/TransactionQueue.h"

namespace {
using namespace cd;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- C1: note-off flood + timed params must never overflow the 4096-event slice ----
void c1QueueOverflow() {
    using namespace cd::state;
    TransactionQueue q;
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n) q.push(TransactionEvent::noteOff(std::uint8_t(ch), std::uint8_t(n), 0));
    for (int i = 0; i < 2100; ++i)
        q.push(TransactionEvent::param(core::kParams[i % 2].hostId, double(i) / 2100.0, std::uint32_t(i % 128)));
    auto s = q.drainSlice(128);  // audio was stalled: everything lands in one slice
    CHECK(s.size() <= kSliceEventCapacity);
    std::size_t noteOffs = 0;
    bool omni = false;
    for (const auto& e : s) {
        if (e.kind == EventKind::NoteOff) ++noteOffs;
        if (e.kind == EventKind::AllNotesOff) omni = true;
    }
    CHECK(noteOffs == 2048 || omni);  // note-offs survive, or release is forced
    // Heavier: more note-offs than the slice can hold plus automation.
    TransactionQueue q2(16384);
    for (int i = 0; i < 6000; ++i) q2.push(TransactionEvent::noteOff(std::uint8_t(i % 16), std::uint8_t(i % 128), 0));
    for (int i = 0; i < 4000; ++i) q2.push(TransactionEvent::param(core::kParams[i % 2].hostId, 0.5, 0));
    auto s2 = q2.drainSlice(128);
    CHECK(s2.size() <= kSliceEventCapacity);
}

// ---- H1: Json.h default constructor is declared out of line (clang+libstdc++ build) ----
void h1JsonCtor() {
    static_assert(std::is_nothrow_default_constructible_v<core::JsonValue>);
    core::JsonValue v;
    CHECK(v.isNull());
}

// ---- M1: undo/redo must not rewind host-automated parameters ----
std::string docWith(double mix, double hpf) {
    static const char* kScene = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"scene","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic scene","tags":[],"createdAt":"2026-10-30T12:00:00Z",
 "requiredFeatures":["CD-F20"],"blueprint":{"schemaVersion":1,"id":"example.rc","components":[{"id":"R1","modelId":"fixture.resistor"}]},
 "baseParameters":{"mix":1,"hpf.frequency":20.5},"macros":{},"timeline":{},"faults":[],"seed":42})";
    state::StateDocument d;
    std::string e;
    CHECK(state::loadState(kScene, d, e));
    for (auto& kv : d.baseParameters) {
        if (kv.first == "mix") kv.second = mix;
        if (kv.first == "hpf.frequency") kv.second = hpf;
    }
    return state::saveState(d);
}
double param(const state::ConfigBlob& b, const char* id) {
    state::StateDocument d;
    std::string e;
    if (!b || !state::loadState(*b, d, e)) return -1;
    for (const auto& kv : d.baseParameters) if (kv.first == id) return kv.second;
    return -1;
}
void m1HistoryAutomation() {
    using namespace cd::state;
    History h([] { return std::int64_t(0); });
    h.load(History::makeBlob(docWith(1, 20.5)));
    CHECK(h.commit("edit hpf", TxOrigin::Gesture, {"hpf.frequency"}, History::makeBlob(docWith(1, 30))) == CommitResult::Recorded);
    CHECK(h.commit("auto", TxOrigin::Automation, {"mix"}, History::makeBlob(docWith(0.5, 30))) == CommitResult::Excluded);
    auto u = h.undo();
    CHECK(u && param(u, "hpf.frequency") == 20.5 && param(u, "mix") == 0.5);  // gesture undone, automation kept
    auto r = h.redo();
    CHECK(r && param(r, "hpf.frequency") == 30 && param(r, "mix") == 0.5);
    // Without automation the stored blobs are restored verbatim.
    History g;
    g.load(History::makeBlob(docWith(1, 20.5)));
    g.commit("e", TxOrigin::Gesture, {"hpf.frequency"}, History::makeBlob(docWith(1, 30)));
    CHECK(*g.undo() == docWith(1, 20.5));
}

// ---- M2: concurrent retries share one RNG ----
struct Always503 final : community::Transport {
    community::TransportResponse send(const community::TransportRequest&, const community::CancelToken&) override {
        community::TransportResponse r;
        r.outcome = community::TransportOutcome::Completed;
        r.status = 503;
        r.body = "{}";
        return r;
    }
};
struct NoSleepClock final : community::Clock {
    std::atomic<std::int64_t> t{0};
    std::int64_t nowMs() override { return t; }
    bool sleepMs(std::int64_t ms, const community::CancelToken&) override { t += ms; return true; }
};
void m2RngRace() {
    Always503 tr;
    NoSleepClock clk;
    community::SeededRng rng(11);
    community::ClientConfig cfg;
    cfg.transport = &tr; cfg.clock = &clk; cfg.rng = &rng;
    community::CatalogClient client(cfg);
    std::atomic<int> unavailable{0};
    auto work = [&] {
        community::CancelToken cx;
        for (int i = 0; i < 200; ++i)
            if (client.listPresets({}, cx).error.kind == community::ErrorKind::Unavailable) ++unavailable;
    };
    std::thread a(work), b(work);
    a.join(); b.join();
    CHECK(unavailable == 400);
}

// ---- M3: sample counters are 64-bit (Windows long is 32-bit) ----
void m3Counters() {
    static_assert(std::is_same_v<decltype(std::declval<blueprint::FaultChannel>().contactTransitions()), std::int64_t>);
    static_assert(std::is_same_v<decltype(std::declval<dsp::MnaSolver>().failures()), std::int64_t>);
}

// ---- L2: invalid manifest bindings -> macro unsupported, finite output ----
void l2MacroBindings() {
    using namespace cd::control;
    BaseValues b; MacroValues m; m.age = 0.5; m.sag = 0.5; m.warmth = 0.5;
    ManifestBindings mb; mb.hasAge = mb.hasSag = mb.hasWarmth = true;
    mb.leakRange = std::nan(""); mb.cMin = std::nan(""); mb.cBase = 1e-4; mb.biasSpan = std::nan("");
    Effective e = evaluate(b, m, mb);
    CHECK(std::isfinite(e.leakFrac) && std::isfinite(e.reservoirC) && std::isfinite(e.biasOffset));
    CHECK(!e.supportsAge && !e.supportsSag && !e.supportsWarmth);
    ManifestBindings neg; neg.hasSag = true; neg.cMin = -1; neg.cBase = 1e-4;
    CHECK(!evaluate(b, m, neg).supportsSag);
    ManifestBindings inv; inv.hasSag = true; inv.cMin = 1e-3; inv.cBase = 1e-4;  // cBase < cMin
    CHECK(!evaluate(b, m, inv).supportsSag);
    ManifestBindings flux; flux.hasWarmth = true; flux.fluxMin = 2; flux.fluxMax = 1;
    CHECK(!evaluate(b, m, flux).supportsWarmth);
    ManifestBindings okb; okb.hasSag = true; okb.cMin = 1e-6; okb.cBase = 1e-4;
    CHECK(evaluate(b, m, okb).supportsSag);
    ManifestBindings mb2; mb2.hasAge = true; mb2.maxTolSigma = 0.6;
    m.age = 1; b.tolSigma = 0.5;
    CHECK(evaluate(b, m, mb2).tolSigma <= 0.25);
    ManifestBindings badTol; badTol.hasAge = true; badTol.maxTolSigma = -1;
    CHECK(!evaluate(b, m, badTol).supportsAge);
}

// ---- L3: elements with other than two pins are rejected, not indexed out of bounds ----
void l3PinCount() {
    blueprint::ModelRegistry reg = blueprint::ModelRegistry::syntheticFixtures();
    for (auto& d : reg.devices) if (d.modelId == "fixture.resistor") d.pins = {"p", "n", "x"};
    const std::string text = R"({"schemaVersion":1,"id":"t.bp","name":"L3","modelRegistryVersion":"fixture-v1","nodes":["0","in","out"],
 "components":[{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out","x":"0"},"parameters":{"resistanceOhm":1000}}],
 "ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})";
    const blueprint::CompileResult r = blueprint::compileText(text, reg, blueprint::RuntimeConfig{8000, 1, 1.0});
    CHECK(!r.ok && !r.graph && !r.diagnostics.empty());
}

// ---- L4/L5/L6: catalog client ----
struct Fail503 final : community::Transport {
    community::TransportResponse send(const community::TransportRequest&, const community::CancelToken&) override {
        community::TransportResponse r;
        r.status = 503;
        return r;
    }
};
void l4LogsStripQuery() {
    Fail503 tr;
    NoSleepClock clk;
    community::SeededRng rng(3);
    std::vector<std::string> logs;
    community::ClientConfig cfg;
    cfg.transport = &tr; cfg.clock = &clk; cfg.rng = &rng;
    cfg.log = [&](const std::string& s) { logs.push_back(s); };
    community::CatalogClient client(cfg);
    community::RevisionInfo info;
    info.blobSha256 = std::string(64, 'a');
    info.byteLength = 100;
    info.downloadUrl = "https://cdn.mock.invalid/blobs/x?X-Signature=SYNTHETIC-PRESIGN-TOKEN#frag-secret";
    community::CancelToken cx;
    client.fetchScene(info, cx);
    CHECK(!logs.empty());
    for (const auto& l : logs)
        CHECK(l.find("SYNTHETIC-PRESIGN-TOKEN") == std::string::npos && l.find("frag-secret") == std::string::npos && l.find('?') == std::string::npos);
}
void l5CacheBounded() {
    community::MockCatalogService svc;
    NoSleepClock clk;
    community::SeededRng rng(5);
    community::ClientConfig cfg;
    cfg.transport = &svc; cfg.clock = &clk; cfg.rng = &rng;
    community::CatalogClient client(cfg);
    community::CancelToken cx;
    for (int i = 0; i < 300; ++i) {
        community::ListQuery q;
        q.q = "query" + std::to_string(i);
        client.listPresets(q, cx);
    }
    CHECK(client.cachedMetadataEntries() > 0 && client.cachedMetadataEntries() <= 64);
    community::LruMap<int> lru(2);
    lru.put("a", 1); lru.put("b", 2); lru.get("a"); lru.put("c", 3);
    CHECK(lru.get("a") && !lru.get("b") && lru.get("c") && lru.size() == 2);
}
struct ThrowThenForward final : community::Transport {
    community::Transport* inner;
    std::atomic<int> throwsLeft{2};
    explicit ThrowThenForward(community::Transport* t) : inner(t) {}
    community::TransportResponse send(const community::TransportRequest& r, const community::CancelToken& c) override {
        if (throwsLeft.fetch_sub(1) > 0) throw std::runtime_error("synthetic transport failure");
        return inner->send(r, c);
    }
};
void l6ThrowingTransport() {
    community::MockCatalogService svc;
    ThrowThenForward tr(&svc);
    NoSleepClock clk;
    community::SeededRng rng(9);
    community::ClientConfig cfg;
    cfg.transport = &tr; cfg.clock = &clk; cfg.rng = &rng;
    community::CatalogClient client(cfg);
    community::CancelToken cx;
    int thrown = 0;
    for (int i = 0; i < 2; ++i) { try { client.listPresets({}, cx); } catch (const std::exception&) { ++thrown; } }
    CHECK(thrown == 2);
    community::CancelToken watchdog;  // a leaked in-flight slot would block forever; cancel after 3 s
    std::atomic<bool> done{false};
    std::thread w([&] {
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        watchdog.cancel();
    });
    auto r = client.listPresets({}, watchdog);
    done = true;
    w.join();
    CHECK(r.ok());
}

// ---- L7: rejected octave jumps must not extend the 200 ms hold ----
void l7PitchHold() {
    using namespace cd::control;
    constexpr double fs = 48000, kPi = 3.14159265358979323846;
    PitchDetector d(fs);
    std::vector<float> sig;
    auto tone = [&](double f, double sec) {
        const std::size_t n = std::size_t(fs * sec), base = sig.size();
        for (std::size_t i = 0; i < n; ++i) sig.push_back(float(0.5 * std::sin(2 * kPi * f * double(base + i) / fs)));
    };
    tone(200, 0.5);
    const std::size_t lockEnd = sig.size();
    {   // geometric chirp 450 Hz -> ~1900 Hz, +5 % per 10.7 ms hop: voiced, far from 200 Hz, never 3 mutually stable hops
        double ph = 0, f = 450;
        const std::size_t n = std::size_t(0.40 * fs);
        for (std::size_t i = 0; i < n; ++i) {
            ph += 2 * kPi * f / fs;
            f = std::min(1900.0, f * std::pow(1.05, 1.0 / (0.010667 * fs)));
            sig.push_back(float(0.5 * std::sin(ph)));
        }
    }
    std::vector<PitchEstimate> all, buf(16);
    for (std::size_t i = 0; i < sig.size(); i += 512) {
        d.submit(sig.data() + i, std::min<std::size_t>(512, sig.size() - i));
        for (std::size_t n; (n = d.process(buf.data(), buf.size())) > 0;)
            all.insert(all.end(), buf.begin(), buf.begin() + std::ptrdiff_t(n));
    }
    bool locked = false, reverted = false;
    for (const auto& e : all) {
        if (e.timestamp < lockEnd && e.valid && std::fabs(e.frequencyHz - 200) < 5) locked = true;
        if (e.timestamp > lockEnd + std::uint64_t(0.27 * fs) && !e.valid) reverted = true;
    }
    CHECK(locked);
    CHECK(reverted);
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    c1QueueOverflow();
    h1JsonCtor();
    m1HistoryAutomation();
    m2RngRace();
    m3Counters();
    l2MacroBindings();
    l3PinCount();
    l4LogsStripQuery();
    l5CacheBounded();
    l6ThrowingTransport();
    l7PitchHold();
    std::printf("review regressions: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
