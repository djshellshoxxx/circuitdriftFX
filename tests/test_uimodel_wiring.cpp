// Tests: UX-01 gesture -> engine wiring (real cd::runtime::Engine), undo/redo through normal ramps,
// automation exclusion, <=120 Hz end-to-end publish, and the telemetry display binding.
// View-model only; fixtures are SYNTHETIC (syntheticDiodeClipper, synthetic telemetry writer);
// nothing here is physical validation (G01 stays open). Owner: uimodel agent.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "analysis/Telemetry.h"
#include "runtime/Engine.h"
#include "state/TransactionQueue.h"
#include "uimodel/Gesture.h"
#include "uimodel/TelemetryBinding.h"
#include "uimodel/Wiring.h"

namespace {
using namespace cd;
using namespace cd::uimodel;
using cd::runtime::AudioBuses;
using cd::runtime::Engine;
using cd::runtime::EngineConfig;
using cd::runtime::ProcessContext;
using cd::state::TransactionEvent;
using cd::state::TransactionQueue;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d [%s]: ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

constexpr double kFs = 48000.0;
constexpr std::uint32_t kBlock = 480;   // 10 ms
using Buf = std::vector<std::vector<double>>;

EngineConfig makeConfig() {
    EngineConfig c;
    c.sampleRate = kFs; c.maxBlock = 512; c.channels = 2;
    c.quality.mode = runtime::QualityMode::Studio;
    c.circuit = runtime::syntheticDiodeClipper();
    return c;
}

// One audio block through the queue path. `amp` 0 => silence.
struct Rig {
    Engine eng;
    TransactionQueue q;
    std::int64_t pos = 0;
    double amp = 0.3;
    std::uint64_t n = 0;     // input phase counter (shared by construction: same sequence per rig)
    Buf out = Buf(2, std::vector<double>(kBlock));
    Buf in = Buf(2, std::vector<double>(kBlock));
    Rig() { std::string e; if (!eng.prepare(makeConfig(), e)) std::printf("prepare failed: %s\n", e.c_str()); }
    const Buf& block() {
        for (std::uint32_t i = 0; i < kBlock; ++i, ++n) {
            const double s = amp * std::sin(2.0 * M_PI * 440.0 * double(n) / kFs);
            in[0][i] = s; in[1][i] = 0.8 * s;
        }
        const double* ip[2] = {in[0].data(), in[1].data()};
        double* op[2] = {out[0].data(), out[1].data()};
        eng.processQueued(ProcessContext{kFs, pos, 0, kBlock, false}, AudioBuses{2, ip, op}, q);
        pos += kBlock;
        return out;
    }
};

double peak(const Buf& a, const Buf& b) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = 0; i < a[c].size(); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}

// Forwards to the engine host and records what the gesture published, so the reference engine can
// be driven with the equivalent direct host-parameter events.
struct Recorder : EditHost {
    EditHost& inner;
    std::vector<std::pair<std::string, double>> performs;
    int begins = 0, ends = 0;
    explicit Recorder(EditHost& h) : inner(h) {}
    void beginEdit(const std::string& id) override { ++begins; inner.beginEdit(id); }
    void performEdit(const std::string& id, double v) override { performs.emplace_back(id, v); inner.performEdit(id, v); }
    void endEdit(const std::string& id) override { ++ends; inner.endEdit(id); }
};

struct Ui {
    ParamDocument doc = ParamDocument::fromRegistry();
    state::History hist;
    std::int64_t nowUs = 1000000;
    TransactionQueue* q = nullptr;
    std::unique_ptr<EngineEditHost> host;
    std::unique_ptr<Recorder> rec;
    std::unique_ptr<GestureController> gc;
    explicit Ui(TransactionQueue& queue) : q(&queue) {
        doc.add("tube.bias", 0, 1, 0.5);
        doc.add("supply.capacitance", 1e-6, 1e-3, 1e-4);
        for (const char* c : {"R1", "R2"}) { doc.add(ageId(c), 0, 1, 0.2); doc.add(noiseLinkId(c), 0, 1, 0.1); }
        host = std::make_unique<EngineEditHost>(queue, [this] { return nowUs; });
        rec = std::make_unique<Recorder>(*host);
        gc = std::make_unique<GestureController>(doc, hist, *rec, [this] { return nowUs; });
        hist.load(doc.blob());
    }
};

// T-UX01-wire: a drag/numeric/step sequence yields bit-identical engine output to direct events.
void gestureMatchesDirectEvents() {
    Rig a, b, idle;
    Ui ui(a.q);
    double worst = 0, worstIdle = 0;
    auto tick = [&]() {
        const Buf& ya = a.block();
        for (const auto& p : ui.rec->performs) {   // equivalent direct host-param event sequence
            const core::ParamSpec* s = core::findById(p.first.c_str());
            if (s) b.q.push(TransactionEvent::param(s->hostId, core::toNormalized(*s, p.second), 0));
        }
        ui.rec->performs.clear();
        const Buf& yb = b.block();
        const Buf& yi = idle.block();
        worst = std::max(worst, peak(ya, yb));
        worstIdle = std::max(worstIdle, peak(ya, yi));
    };
    for (int i = 0; i < 20; ++i) tick();   // settle, same on all three
    // Supply pinch: sag (registry id) + capacitance (view-model only: unbound here).
    CHECK(ui.gc->beginPinch({100, 100}, {200, 100}), "begin pinch");
    for (int i = 1; i <= 40; ++i) {
        ui.nowUs += 10000;
        ui.gc->movePinch({100.0 - i, 100}, {200.0 + 2 * i, 100});
        tick();
    }
    CHECK(ui.gc->end(), "end pinch");
    tick();
    CHECK(ui.gc->enterNumeric("mix", "40%"), "mix entry");
    tick(); tick();
    CHECK(ui.gc->step("input.gain", 1, false), "gain step");
    for (int i = 0; i < 30; ++i) tick();
    CHECK(ui.gc->enterNumeric("hpf.frequency", "40 Hz"), "hpf entry");
    for (int i = 0; i < 30; ++i) tick();
    CHECK(worst == 0.0, "gesture vs direct events not bit-identical: %.3e", worst);
    CHECK(worstIdle > 1e-4, "edits had no audible effect on the engine (%.3e): test would be vacuous", worstIdle);
    CHECK(ui.host->stats().pushed > 40 && ui.host->openEdits() == 0 && ui.host->stats().protocolErrors == 0,
          "pushed %lld open %zu protoErr %lld", (long long)ui.host->stats().pushed, ui.host->openEdits(), (long long)ui.host->stats().protocolErrors);
    CHECK(ui.rec->begins == ui.rec->ends && ui.rec->begins >= 5, "begin/end pairing %d/%d", ui.rec->begins, ui.rec->ends);
    // view-model-only ids are reported, never silently dropped.
    const auto& u = ui.host->unboundIds();
    CHECK(std::find(u.begin(), u.end(), "supply.capacitance") != u.end() && ui.host->stats().unbound >= 1, "supply.capacitance must be reported unbound");
    CHECK(a.eng.diagnostics().paramEvents == ui.host->stats().pushed, "engine saw %lld param events, host pushed %lld",
          (long long)a.eng.diagnostics().paramEvents, (long long)ui.host->stats().pushed);
    std::printf("gesture wiring: worst |gesture - direct| = %.1f (bit-identical), effect vs idle engine = %.3e, %lld events, unbound %zu id(s)\n",
                worst, worstIdle, (long long)ui.host->stats().pushed, u.size());
}

// Routing: registry / mod slot / blueprint / unbound, with sad paths.
void routing() {
    TransactionQueue q;
    EngineEditHost h(q);
    std::vector<std::string> seen;
    h.setUnboundObserver([&](const std::string& id) { seen.push_back(id); });
    CHECK(h.routeOf("mix") == Route::Registry && h.routeOf("tube.bias") == Route::Unbound, "routes");
    CHECK(!h.bindModSlot("mix", core::ParamScope::SlotA, 1, 0, 1), "registry ids cannot be rebound");
    CHECK(!h.bindModSlot("tube.bias", core::ParamScope::SlotA, 0, 0, 1) && !h.bindModSlot("tube.bias", core::ParamScope::SlotA, 17, 0, 1) &&
              !h.bindModSlot("tube.bias", core::ParamScope::Global, 1, 0, 1) && !h.bindModSlot("tube.bias", core::ParamScope::SlotA, 1, 1, 1) &&
              !h.bindModSlot("tube.bias", core::ParamScope::SlotA, 1, 0, NAN), "invalid mod slot bindings");
    CHECK(h.bindModSlot("tube.bias", core::ParamScope::SlotA, 3, 0, 2), "bind mod slot");
    CHECK(h.routeOf("tube.bias") == Route::ModSlot, "mod route");
    double sunk = -1;
    CHECK(h.bindBlueprint("material.D1", [&](const std::string&, double v) { sunk = v; return true; }), "bind blueprint");
    CHECK(!h.bindBlueprint("mix", [](const std::string&, double) { return true; }) && !h.bindBlueprint("x", nullptr), "bad blueprint bind");
    h.beginEdit("tube.bias");
    h.performEdit("tube.bias", 1.0);   // -> slot.a.mod.03 normalized 0.5
    h.endEdit("tube.bias");
    h.beginEdit("material.D1");
    h.performEdit("material.D1", 2);
    h.endEdit("material.D1");
    h.beginEdit("age.R9");
    h.performEdit("age.R9", 0.3);
    h.endEdit("age.R9");
    h.beginEdit("mix"); h.performEdit("mix", NAN); h.performEdit("mix", 0.25); h.endEdit("mix");
    h.performEdit("mix", 0.5);   // no begin: delivered but counted
    h.endEdit("never");          // end without begin
    h.beginEdit("mix"); h.beginEdit("mix");   // nested
    CHECK(sunk == 2 && h.stats().blueprintEdits == 1, "blueprint sink");
    CHECK(h.unboundIds().size() == 1 && h.unboundIds()[0] == "age.R9" && seen == h.unboundIds() && h.stats().unbound == 1, "unbound report");
    CHECK(h.stats().badValues == 1 && h.stats().protocolErrors == 3, "bad %lld proto %lld", (long long)h.stats().badValues, (long long)h.stats().protocolErrors);
    const auto ev = q.drainSlice(64);
    CHECK(ev.size() == 3, "events %zu", ev.size());
    if (ev.size() == 3) {
        CHECK(ev[0].hostId == 1102 && std::fabs(ev[0].normalized - 0.5) < 1e-15, "mod slot 3 host id / mapping (%u, %g)", ev[0].hostId, ev[0].normalized);
        CHECK(ev[1].hostId == 1007 && std::fabs(ev[1].normalized - 0.25) < 1e-15, "mix event");
    }
    // Mod slot value clamps into the slot range.
    h.performEdit("tube.bias", 99.0);
    const auto e2 = q.drainSlice(64);
    CHECK(e2.size() == 1 && e2[0].normalized == 1.0, "clamped mod slot");
}

// T-UX01-undo + T-UX01-ramp: undo returns the engine to config equality via normal ramps.
void undoReturnsToConfig() {
    Rig a, fresh;
    a.amp = fresh.amp = 0.0;   // silence while editing: no signal-dependent (thermal/capacitor) state differs
    Ui ui(a.q);
    auto both = [&](int blocks) { for (int i = 0; i < blocks; ++i) { a.block(); fresh.block(); } };
    both(10);
    CHECK(ui.gc->beginPinch({100, 100}, {200, 100}), "pinch");
    for (int i = 1; i <= 20; ++i) { ui.nowUs += 10000; ui.gc->movePinch({100.0 - i, 100}, {200.0 + i, 100}); both(1); }
    ui.gc->end();
    CHECK(ui.gc->enterNumeric("mix", "30%"), "mix");
    both(30);
    CHECK(ui.gc->step("input.gain", -1, false), "gain");
    CHECK(ui.gc->enterNumeric("hpf.frequency", "120 Hz"), "hpf");
    both(30);
    CHECK(ui.hist.undoDepth() == 4, "undo depth %zu", ui.hist.undoDepth());
    const double mixBefore = a.eng.diagnostics().output.mix;
    CHECK(std::fabs(mixBefore - 0.3) < 1e-6, "mix settled %.9g", mixBefore);

    EngineEditHost& host = *ui.host;
    UndoRedoController undo(ui.doc, ui.hist, host);
    const auto histBefore = ui.hist.undoDepth();
    // Undo only the last edit first: it changes hpf.frequency only (exactly the changed values are published).
    CHECK(undo.undo() && undo.lastPublished().size() == 1 && undo.lastPublished()[0] == "hpf.frequency", "undo 1 published %zu", undo.lastPublished().size());
    CHECK(ui.hist.undoDepth() == histBefore - 1 && ui.hist.redoDepth() == 1, "undo is not a new history entry");
    both(1);
    // Normal ramp: one 10 ms block after undoing the mix edit has NOT jumped to the target.
    CHECK(undo.undo() && undo.undo() == true, "undo 2,3");   // gain step + mix entry
    const double mixTarget = ui.doc.get("mix");
    CHECK(mixTarget == 1.0, "mix restored %.3f", mixTarget);
    both(1);
    const double mixAfter1 = a.eng.diagnostics().output.mix;
    CHECK(mixAfter1 > 0.3 + 1e-6 && mixAfter1 < 1.0 - 1e-6, "undo must ramp, not jump: mix after 10 ms = %.6f", mixAfter1);
    while (ui.hist.undoDepth() > 0) CHECK(undo.undo(), "undo chain");
    CHECK(!undo.undo() && undo.lastPublished().empty(), "nothing left to undo");
    ui.rec->performs.clear();
    both(150);   // settle 1.5 s
    // Config equality: effective parameter state and rendered output match a never-edited engine.
    const auto da = a.eng.diagnostics(), df = fresh.eng.diagnostics();
    CHECK(std::fabs(da.output.mix - df.output.mix) <= 1e-9 && std::fabs(da.output.inputGain - df.output.inputGain) <= 1e-9 &&
              std::fabs(da.output.trim - df.output.trim) <= 1e-9 && std::fabs(da.output.hpfCutoffHz - df.output.hpfCutoffHz) <= 1e-9 &&
              std::fabs(da.macros.sag - df.macros.sag) <= 1e-9 && std::fabs(da.driveGain - df.driveGain) <= 1e-9,
          "effective state differs: mix %.12g/%.12g gain %.12g/%.12g hpf %.12g/%.12g sag %.12g/%.12g",
          da.output.mix, df.output.mix, da.output.inputGain, df.output.inputGain, da.output.hpfCutoffHz, df.output.hpfCutoffHz,
          da.macros.sag, df.macros.sag);
    a.amp = fresh.amp = 0.3;
    double worst = 0;
    for (int i = 0; i < 40; ++i) { const Buf& ya = a.block(); const Buf& yf = fresh.block(); worst = std::max(worst, peak(ya, yf)); }
    CHECK(worst <= 1e-9, "render after undo vs fresh engine: %.3e", worst);
    std::printf("undo: restored engine vs fresh engine render diff %.3e (limit 1e-9); mix after first 10 ms ramp %.4f\n", worst, mixAfter1);
    // Redo republishes the redone values and the document follows.
    CHECK(undo.redo() && ui.doc.get("supply.capacitance") != 1e-4 && ui.hist.redoDepth() == 3, "redo");
    CHECK(std::find(host.unboundIds().begin(), host.unboundIds().end(), "supply.capacitance") != host.unboundIds().end(), "capacitance undo/redo reported unbound");
}

// T-UX01-auto: host automation updates the live document but never enters history or the queue.
void automationExcluded() {
    Rig a;
    Ui ui(a.q);
    CHECK(ui.gc->enterNumeric("mix", "70%"), "edit");
    CHECK(a.q.drainSlice(16).size() == 1, "the user edit itself was queued");
    const auto depth = ui.hist.undoDepth();
    CHECK(depth == 1, "depth");
    CHECK(applyHostAutomation(ui.doc, ui.hist, 1007, 0.2), "automation");
    CHECK(ui.hist.undoDepth() == depth && ui.hist.redoDepth() == 0, "automation changed history");
    CHECK(std::fabs(ui.doc.get("mix") - 0.2) < 1e-12 && *ui.hist.current() == ui.doc.serialize(), "live document must follow automation");
    CHECK(a.q.drainSlice(16).empty() && ui.host->stats().pushed == 1, "automation must not be echoed to the engine queue");
    CHECK(!applyHostAutomation(ui.doc, ui.hist, 4242, 0.5) && !applyHostAutomation(ui.doc, ui.hist, 1007, NAN) && !applyHostAutomation(ui.doc, ui.hist, 1007, 1.5),
          "bad automation refused");
    CHECK(ui.hist.commit("x", state::TxOrigin::Automation, {"mix"}, ui.doc.blob()) == state::CommitResult::Excluded, "origin excluded");
    // Undo of the earlier user edit still works and only touches what differs from the live (automated) value.
    UndoRedoController undo(ui.doc, ui.hist, *ui.host);
    CHECK(undo.undo() && ui.hist.undoDepth() == 0, "undo user edit after automation");
}

// T-UX01-rate: end-to-end publish <= 120 Hz while a gesture is driven at 1 kHz.
void publishRate() {
    Rig a;
    Ui ui(a.q);
    CHECK(ui.gc->beginPinch({100, 100}, {200, 100}), "pinch");
    for (int i = 1; i <= 3000; ++i) {   // 3 s of 1 kHz pointer events
        ui.nowUs += 1000;
        ui.gc->movePinch({100 - 0.01 * i, 100}, {200 + 0.01 * i, 100});
        if (i % 10 == 0) a.block();   // audio drains the queue every 10 ms
    }
    ui.gc->end();
    a.block();
    const int peakHz = ui.host->peakPublishesPerSecond();
    CHECK(peakHz <= 120 && peakHz > 60, "peak publishes per second per id = %d", peakHz);
    const auto d = a.eng.diagnostics();
    CHECK(d.paramEvents == ui.host->stats().pushed && ui.host->stats().pushed <= 3 * 120 + 2, "engine events %lld host pushed %lld",
          (long long)d.paramEvents, (long long)ui.host->stats().pushed);
    CHECK(d.eventsCoalesced == 0, "no coalescing at 120 Hz");
    std::printf("publish rate: peak %d Hz per id (1 kHz pointer input), %lld events in 3 s\n", peakHz, (long long)ui.host->stats().pushed);
}

// ---------------------------------------------------------------- telemetry binding
analysis::TelemetryFrame frame(std::uint32_t gen, std::int64_t sample, double kelvin = 358.15) {
    analysis::TelemetryFrame f;
    f.graphGeneration = gen; f.sampleIndex = sample; f.epoch = 1;
    f.validityFlags = analysis::kValidRail;
    f.railVolts = 250.0f; f.railNominalVolts = 300.0f; f.railFraction = 250.0f / 300.0f;
    f.componentCount = 3;
    f.components[0] = {10, 1u, float(kelvin), 0.5f, 0.0125f, 123.5f};
    f.components[1] = {11, 0u, 400.0f, 9.0f, 9.0f, 9.0f};   // invalid record: must be skipped
    f.components[2] = {12, 1u, 300.15f, 0.0f, 0.0f, 0.0f};
    return f;
}

void telemetryValues() {
    analysis::TelemetryChannel ch(64);
    analysis::TelemetryWriter w(ch);
    analysis::TelemetryReader r(ch);
    TelemetryBinding b(r);
    CHECK(!b.update(0.0) && b.view().stale && !b.view().haveFrame, "nothing yet is stale");
    w.publish(frame(1, 100));
    CHECK(b.update(1.0) && b.view().haveFrame && !b.view().stale && !b.view().gap, "first frame");
    const TelemetryView& v = b.view();
    CHECK(v.components.size() == 2 && v.components[0].componentId == 10 && v.components[1].componentId == 12, "invalid record skipped");
    if (v.components.size() == 2) {
        CHECK(std::fabs(v.components[0].temperature.value - 85.0) < 1e-3 && std::string(v.components[0].temperature.unit) == "\xC2\xB0" "C" &&
                  v.components[0].temperature.text() == "85.0 \xC2\xB0" "C", "temperature %s", v.components[0].temperature.text().c_str());
        CHECK(v.components[0].voltage.text() == "123.50 V" && v.components[0].current.text() == "12.5 mA" && v.components[0].dissipation.text() == "0.50 W",
              "units: %s / %s / %s", v.components[0].voltage.text().c_str(), v.components[0].current.text().c_str(), v.components[0].dissipation.text().c_str());
        CHECK(v.components[1].current.text() == "0.000 A" && v.components[1].temperature.text() == "27.0 \xC2\xB0" "C", "zero current / 300.15 K");
    }
    CHECK(v.railValid && v.railVolts.text() == "250.00 V" && v.railNominalVolts.text() == "300.00 V" && std::fabs(v.railFraction - 250.0 / 300.0) < 1e-6, "rail");
    CHECK(DisplayValue{}.text() == "--" && formatVolts(NAN) == "--" && formatTemperatureC(INFINITY) == "--", "invalid shows --");
    // Stale: no extrapolation, values retained but flagged.
    b.update(1.0 + analysis::kStaleAfterSeconds + 0.01);
    CHECK(b.view().stale && b.view().components.size() == 2 && b.view().sampleIndex == 100, "stale flag, values kept");
    // Gap: writer sequence advances for dropped frames.
    analysis::TelemetryChannel small(4);
    analysis::TelemetryWriter ws(small);
    analysis::TelemetryReader rs(small);
    TelemetryBinding bs(rs);
    int dropped = 0;
    for (int i = 0; i < 40; ++i) dropped += ws.publish(frame(1, i)) ? 0 : 1;
    CHECK(bs.update(2.0) && dropped > 0, "drained");
    for (int i = 0; i < 3; ++i) ws.publish(frame(1, 1000 + i));
    CHECK(bs.update(2.1) && bs.view().sampleIndex == 1002, "newest frame wins");
    CHECK(bs.view().totalMissed > 0, "missed frames accumulated %llu", (unsigned long long)bs.view().totalMissed);
    // Generation change clears prior values (no stale frame crosses a graph generation).
    w.publish(frame(1, 200));
    b.update(3.0);
    w.publish(frame(2, 300));
    b.update(3.01);
    CHECK(b.view().generationChanged && b.view().tracesCleared == 1 && b.view().graphGeneration == 2 && b.view().sampleIndex == 300, "generation change");
    // Schema mismatch: nothing displayed from the frame.
    analysis::TelemetryFrame bad = frame(2, 400);
    w.publish(bad);
    b.update(3.02);
    // (writer stamps the schema itself, so a mismatch cannot be injected here; the display stays valid)
    CHECK(!b.view().schemaMismatch && b.view().components.size() == 2, "writer-stamped schema");
    // Fallback flags.
    analysis::TelemetryFrame fb = frame(2, 500);
    fb.fallbackFlags = analysis::kFallbackSolverDiverged | analysis::kFallbackLimiterActive;
    w.publish(fb);
    b.update(3.03);
    CHECK(b.view().solverFallback && b.view().limiterActive && !b.view().bypassed, "fallback flags");
    // Oversized componentCount is clamped, never read out of bounds.
    analysis::TelemetryFrame big = frame(2, 600);
    big.componentCount = 9999;
    w.publish(big);
    b.update(3.04);
    CHECK(b.view().components.size() <= std::size_t(analysis::kMaxTelemetryComponents), "component clamp");
    b.detach();
    CHECK(!b.attached() && !b.update(4.0) && b.view().stale, "detached");
    w.publish(frame(2, 700));
    CHECK(w.discardedNoReader() >= 1, "writer discards after close");
}

// T-UX10-gui: closing or stalling the GUI never changes engine output.
void telemetryDoesNotAffectEngine() {
    auto render = [&](int mode) {   // 0 no channel, 1 live reader, 2 closed GUI, 3 stalled GUI
        Rig rg;
        analysis::TelemetryChannel ch(8);
        analysis::TelemetryWriter w(ch);
        analysis::TelemetryReader reader(ch);
        TelemetryBinding bind(reader);
        if (mode == 2) bind.detach();
        Buf all(2);
        for (int i = 0; i < 300; ++i) {
            if (i == 100) { rg.q.push(TransactionEvent::param(1007, 0.5, 100)); }
            const Buf& y = rg.block();
            for (int c = 0; c < 2; ++c) all[std::size_t(c)].insert(all[std::size_t(c)].end(), y[std::size_t(c)].begin(), y[std::size_t(c)].end());
            if (mode != 0) {   // synthetic writer driven by engine diagnostics (the Engine itself may not publish yet)
                const auto d = rg.eng.diagnostics();
                analysis::TelemetryFrame f = frame(1, rg.pos, 300.0 + 40.0 * d.output.mix);
                w.publish(f);
            }
            if (mode == 1) bind.update(0.01 * i);
            // mode 3: never polled -> queue fills, writer drops
        }
        if (mode == 2) CHECK(w.discardedNoReader() > 0, "closed GUI: writer discards");
        if (mode == 3) CHECK(w.droppedFull() > 0, "stalled GUI: writer drops");
        if (mode == 1) CHECK(bind.view().haveFrame, "live binding saw frames");
        return all;
    };
    const Buf base = render(0);
    for (int m = 1; m <= 3; ++m) CHECK(peak(base, render(m)) == 0.0, "mode %d changed engine output", m);
    std::printf("telemetry: engine render bit-identical with live/closed/stalled GUI binding\n");
}

}  // namespace

int main() {
    gestureMatchesDirectEvents();
    routing();
    undoReturnsToConfig();
    automationExcluded();
    publishRate();
    telemetryValues();
    telemetryDoesNotAffectEngine();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("uimodel wiring: OK\n");
    return 0;
}
