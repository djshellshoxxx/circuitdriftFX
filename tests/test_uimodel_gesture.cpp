// Tests: UX-01 gesture mapping table, numeric entry, rollback, undo, publish rate (view-model only;
// SYNTHETIC targets, no physical validation claimed). Owner: uimodel agent.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "uimodel/Gesture.h"

namespace {
using namespace cd;
using namespace cd::uimodel;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d [%s]: ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

struct Host : EditHost {
    struct Ev { char kind; std::string id; double v; std::int64_t t; };
    std::vector<Ev> ev;
    std::int64_t* clock = nullptr;
    void beginEdit(const std::string& id) override { ev.push_back({'b', id, 0, clock ? *clock : 0}); }
    void performEdit(const std::string& id, double v) override { ev.push_back({'p', id, v, clock ? *clock : 0}); }
    void endEdit(const std::string& id) override { ev.push_back({'e', id, 0, clock ? *clock : 0}); }
    int count(char k) const { int n = 0; for (auto& e : ev) n += e.kind == k; return n; }
};

struct Rig {
    ParamDocument doc = ParamDocument::fromRegistry();
    state::History hist;
    Host host;
    std::int64_t now = 1000000;
    GestureController gc;
    static ParamDocument& prep(ParamDocument& d, double bias, double cap, double sag) {
        d.add("tube.bias", 0, 1, bias);
        d.add("supply.capacitance", 1e-6, 1e-3, cap);
        d.set("macro.sag", sag);
        for (const char* c : {"R1", "R2", "R3"}) { d.add(ageId(c), 0, 1, 0.2); d.add(noiseLinkId(c), 0, 1, 0.1); }
        return d;
    }
    explicit Rig(double bias = 0.5, double cap = 1e-4, double sag = 0.5, double scale = 1.0)
        : gc(prep(doc, bias, cap, sag), hist, host, [this] { return now; }, [&] { GestureConfig c; c.uiScale = scale; return c; }()) {
        host.clock = &now;
        hist.load(doc.blob());
    }
};

void testBias() {
    {   // UX-01: 200 logical-pixel drag spans the full normalized bias.
        Rig r(0.0);
        CHECK(r.gc.beginBias({100, 500}), "begin");
        for (int i = 1; i <= 20; ++i) { r.now += 10000; r.gc.moveBias({100, 500 - 10.0 * i}, false); }
        CHECK(r.gc.end(), "end");
        CHECK(std::fabs(r.doc.get("tube.bias") - 1.0) < 1e-12, "full span %.17g", r.doc.get("tube.bias"));
    }
    {   // downward drag decreases; 100 px = 0.5
        Rig r(1.0);
        r.gc.beginBias({0, 0});
        r.now += 10000; r.gc.moveBias({0, 100}, false);
        r.gc.end();
        CHECK(std::fabs(r.doc.get("tube.bias") - 0.5) < 1e-12, "down %.17g", r.doc.get("tube.bias"));
    }
    {   // Shift = x0.1
        Rig r(0.0);
        r.gc.beginBias({0, 300});
        r.now += 10000; r.gc.moveBias({0, 100}, true);
        r.gc.end();
        CHECK(std::fabs(r.doc.get("tube.bias") - 0.1) < 1e-12, "shift %.17g", r.doc.get("tube.bias"));
    }
    {   // dy is divided by UI scale once: 400 device px at scale 2 = full span
        Rig r(0.0, 1e-4, 0.5, 2.0);
        r.gc.beginBias({0, 400});
        r.now += 10000; r.gc.moveBias({0, 0}, false);
        r.gc.end();
        CHECK(std::fabs(r.doc.get("tube.bias") - 1.0) < 1e-12, "scale2 %.17g", r.doc.get("tube.bias"));
    }
    {   // overshoot clamps; returning continues from the unclamped accumulator
        Rig r(0.5);
        r.gc.beginBias({0, 1000});
        r.now += 10000; r.gc.moveBias({0, 400}, false);  // raw 0.5+3 -> clamp 1
        CHECK(r.doc.get("tube.bias") == 1.0, "clamped high");
        r.now += 10000; r.gc.moveBias({0, 1000}, false); // back to raw 0.5 (start)
        r.gc.end();
        CHECK(std::fabs(r.doc.get("tube.bias") - 0.5) < 1e-12, "returns %.17g", r.doc.get("tube.bias"));
    }
    {   // sad paths: NaN/Inf ignored, double begin refused, moves without gesture are no-ops
        Rig r(0.5);
        r.gc.moveBias({0, 10}, false);
        CHECK(r.doc.get("tube.bias") == 0.5, "idle move ignored");
        CHECK(!r.gc.beginBias({std::nan(""), 0}), "NaN begin refused");
        CHECK(r.gc.beginBias({0, 0}), "begin");
        CHECK(!r.gc.beginBias({0, 0}), "second begin refused");
        r.gc.moveBias({0, std::nan("")}, false);
        r.gc.moveBias({INFINITY, 0}, false);
        CHECK(r.doc.get("tube.bias") == 0.5, "non-finite move ignored");
        CHECK(!r.gc.end() == false, "end ok");
        CHECK(!r.gc.end(), "end when idle");
        CHECK(!r.gc.cancel(), "cancel when idle");
    }
    {   // pointer loss commits the last valid value and ends host edits (no stuck automation)
        Rig r(0.0);
        r.gc.beginBias({0, 100});
        r.now += 10000; r.gc.moveBias({0, 0}, false);
        CHECK(r.gc.pointerLost(), "lost");
        CHECK(std::fabs(r.doc.get("tube.bias") - 0.5) < 1e-12, "kept");
        CHECK(r.host.count('b') == r.host.count('e'), "balanced edits");
        CHECK(r.hist.undoDepth() == 1, "committed");
        CHECK(!r.gc.active(), "inactive");
    }
}

void testPinch() {
    {   // pinch ratio sqrt(2) doubles C; Sag delta -log2(sqrt2)/4 = -0.125
        Rig r(0.5, 1e-4, 0.5);
        CHECK(!r.gc.beginPinch({0, 0}, {0, 0}), "zero distance refused");
        CHECK(r.gc.beginPinch({0, 0}, {100, 0}), "begin");
        r.now += 10000; r.gc.movePinch({0, 0}, {100 * std::sqrt(2.0), 0});
        r.gc.end();
        CHECK(std::fabs(r.doc.get("supply.capacitance") / 1e-4 - 2.0) < 1e-12, "C doubled %.17g", r.doc.get("supply.capacitance"));
        CHECK(std::fabs(r.doc.get("macro.sag") - 0.375) < 1e-12, "sag %.17g", r.doc.get("macro.sag"));
        CHECK(r.host.count('b') == 2 && r.host.count('e') == 2, "one transaction over C and Sag");
        CHECK(r.hist.undoDepth() == 1, "one undo entry");
    }
    {   // r clamped 0.5..2 per gesture
        Rig r(0.5, 1e-4, 0.5);
        r.gc.beginPinch({0, 0}, {100, 0});
        r.now += 10000; r.gc.movePinch({0, 0}, {10000, 0});
        CHECK(std::fabs(r.doc.get("supply.capacitance") - 4e-4) < 1e-16, "r=2 -> x4");
        CHECK(std::fabs(r.doc.get("macro.sag") - 0.25) < 1e-12, "sag -0.25");
        r.now += 10000; r.gc.movePinch({0, 0}, {0.001, 0});
        CHECK(std::fabs(r.doc.get("supply.capacitance") - 2.5e-5) < 1e-16, "r=0.5 -> x0.25");
        CHECK(std::fabs(r.doc.get("macro.sag") - 0.75) < 1e-12, "sag +0.25");
        r.gc.cancel();
    }
    {   // manifest bounds and 0..1 clamp
        Rig r(0.5, 8e-4, 0.95);
        r.gc.beginPinch({0, 0}, {100, 0});
        r.now += 10000; r.gc.movePinch({0, 0}, {200, 0});
        CHECK(r.doc.get("supply.capacitance") == 1e-3, "C clamped to manifest max");
        CHECK(r.doc.get("macro.sag") == 0.95 - 0.25 || std::fabs(r.doc.get("macro.sag") - 0.7) < 1e-12, "sag moved down");
        r.gc.end();
        Rig q(0.5, 1e-4, 0.05);
        q.gc.beginPinch({0, 0}, {100, 0});
        q.now += 10000; q.gc.movePinch({0, 0}, {200, 0});
        CHECK(q.doc.get("macro.sag") == 0.0, "sag clamped to 0");
        q.gc.end();
    }
}

void testRollbackUndoRate() {
    {   // Escape rollback is exact and records nothing
        Rig r(0.3137, 1e-4, 0.5);
        const std::string before = r.doc.serialize();
        r.gc.beginBias({0, 0});
        for (int i = 1; i <= 30; ++i) { r.now += 9000; r.gc.moveBias({0, -3.7 * i}, i % 2 == 0); }
        CHECK(r.doc.get("tube.bias") != 0.3137, "moved");
        CHECK(r.gc.cancel(), "cancel");
        CHECK(r.doc.serialize() == before, "document restored bit-exactly");
        CHECK(r.doc.get("tube.bias") == 0.3137, "exact start value");
        CHECK(r.hist.undoDepth() == 0, "no history entry");
        CHECK(r.host.count('b') == 1 && r.host.count('e') == 1, "edit transaction closed");
        CHECK(r.host.ev[r.host.ev.size() - 2].kind == 'p' && r.host.ev[r.host.ev.size() - 2].v == 0.3137, "host told the start value");
        Rig p(0.5);  // rollback of a pinch
        const std::string b2 = p.doc.serialize();
        p.gc.beginPinch({0, 0}, {100, 0});
        p.now += 10000; p.gc.movePinch({0, 0}, {160, 0});
        p.gc.cancel();
        CHECK(p.doc.serialize() == b2, "pinch rollback exact");
    }
    {   // one gesture = one undo transaction
        Rig r(0.2);
        const std::string before = r.doc.serialize();
        r.gc.beginBias({0, 200});
        for (int i = 1; i <= 50; ++i) { r.now += 5000; r.gc.moveBias({0, 200 - 2.0 * i}, false); }
        r.gc.end();
        CHECK(r.hist.undoDepth() == 1, "depth %zu", r.hist.undoDepth());
        const auto blob = r.hist.undo();
        CHECK(blob && r.doc.restore(*blob) && r.doc.serialize() == before, "undo restores pre-gesture state");
        CHECK(r.hist.redoDepth() == 1, "redo available");
    }
    {   // publish rate <= 120 Hz during moves (1 kHz input), final value exact at end
        Rig r(0.0);
        r.gc.beginBias({0, 0});
        const std::int64_t t0 = r.now;
        for (int i = 1; i <= 3000; ++i) { r.now = t0 + i * 1000; r.gc.moveBias({0, -0.05 * i}, false); }
        std::vector<std::int64_t> ts;
        for (auto& e : r.host.ev) if (e.kind == 'p') ts.push_back(e.t);
        std::size_t worst = 0;
        for (std::size_t i = 0; i < ts.size(); ++i) {
            std::size_t j = i;
            while (j < ts.size() && ts[j] - ts[i] < 1000000) ++j;
            worst = std::max(worst, j - i);
        }
        CHECK(worst <= 120, "max publishes in any 1 s window = %zu", worst);
        CHECK(ts.size() > 300, "publishing is not starved (%zu in 3 s)", ts.size());
        r.gc.end();
        CHECK(std::fabs(r.host.ev[r.host.ev.size() - 2].v - 0.0 - r.doc.get("tube.bias")) < 1e-15 ||
              r.host.ev[r.host.ev.size() - 2].v == r.doc.get("tube.bias"), "terminal value published");
        std::printf("uimodel: max publishes/s = %zu\n", worst);
    }
}

void testBrushMaterialNumeric() {
    {   // age brush: frozen set, path/400, Alt reverses, noise linked
        Rig r;
        CHECK(!r.gc.beginBrush({0, 0}, {"nope"}, false), "unknown ids -> no gesture");
        std::vector<std::string> ids = {"R1", "R2", "ghost"};
        CHECK(r.gc.beginBrush({0, 0}, ids, false), "begin");
        ids.push_back("R3");  // caller mutation must not change the frozen set
        CHECK(r.gc.brushTargets().size() == 2, "frozen at pointer-down");
        r.now += 10000; r.gc.moveBrush({100, 0});
        r.now += 10000; r.gc.moveBrush({100, 100});
        r.gc.end();
        CHECK(std::fabs(r.doc.get(ageId("R1")) - (0.2 + 0.5)) < 1e-12, "age +200/400");
        CHECK(std::fabs(r.doc.get(noiseLinkId("R2")) - (0.1 + 0.5)) < 1e-12, "noise linked");
        CHECK(r.doc.get(ageId("R3")) == 0.2, "unrelated component untouched");
        CHECK(r.hist.undoDepth() == 1, "one undo");
        Rig a;
        a.gc.beginBrush({0, 0}, {"R1"}, true);
        a.now += 10000; a.gc.moveBrush({80, 0});
        a.gc.end();
        CHECK(std::fabs(a.doc.get(ageId("R1")) - 0.0) < 1e-12, "Alt reverses (clamped at 0)");
        Rig s(0.5, 1e-4, 0.5, 2.0);
        s.gc.beginBrush({0, 0}, {"R1"}, false);
        s.now += 10000; s.gc.moveBrush({160, 0});  // 80 logical px
        s.gc.end();
        CHECK(std::fabs(s.doc.get(ageId("R1")) - (0.2 + 0.2)) < 1e-12, "path in logical px");
    }
    {   // material: explicit enum, applied once, no interpolation, one undo
        Rig r;
        r.doc.add("material.R1", 0, 2, 0, true);
        r.hist.load(r.doc.blob());
        CHECK(r.gc.selectMaterial("material.R1", 2), "select");
        CHECK(r.host.count('p') == 1 && r.host.ev[1].v == 2.0, "single exact publish");
        CHECK(r.doc.get("material.R1") == 2.0, "value");
        CHECK(!r.gc.selectMaterial("material.R1", 3), "out of range refused");
        CHECK(!r.gc.selectMaterial("material.R1", -1), "negative refused");
        CHECK(r.hist.undoDepth() == 1, "one undo");
    }
    {   // numeric entry: units/range errors leave state untouched
        Rig r;
        const std::string before = r.doc.serialize();
        std::string err;
        CHECK(!r.gc.enterNumeric("input.gain", "99 dB", &err) && !err.empty(), "range error: %s", err.c_str());
        CHECK(!r.gc.enterNumeric("input.gain", "3 Hz", &err), "wrong unit: %s", err.c_str());
        CHECK(!r.gc.enterNumeric("input.gain", "abc", &err), "garbage");
        CHECK(!r.gc.enterNumeric("input.gain", "nan", &err), "nan");
        CHECK(!r.gc.enterNumeric("input.gain", "1e999", &err), "overflow");
        CHECK(!r.gc.enterNumeric("input.gain", "", &err), "empty");
        CHECK(!r.gc.enterNumeric("no.such", "1", &err), "unknown");
        CHECK(!r.gc.enterNumeric("hpf.frequency", "1.5 kHz", &err), "kHz out of range");
        CHECK(r.doc.serialize() == before && r.hist.undoDepth() == 0 && r.host.ev.empty(), "no state change on error");
        CHECK(r.gc.enterNumeric("input.gain", "-6 dB", &err), "ok");
        CHECK(r.doc.get("input.gain") == -6.0, "value");
        CHECK(r.gc.enterNumeric("hpf.frequency", "0.1 kHz", &err) && std::fabs(r.doc.get("hpf.frequency") - 100) < 1e-9, "kHz parse");
        CHECK(r.gc.enterNumeric("mix", "40%", &err) && std::fabs(r.doc.get("mix") - 0.4) < 1e-12, "percent");
        CHECK(r.gc.enterNumeric("hpf.enabled", "off", &err) && r.doc.get("hpf.enabled") == 0.0, "bool word");
        CHECK(!r.gc.enterNumeric("quality.oversampling", "1.5", &err), "enum needs whole number");
        CHECK(r.hist.undoDepth() == 4, "one undo per accepted entry (%zu)", r.hist.undoDepth());
    }
    {   // arrows +-1% / +-0.1%, Home resets factory base
        Rig r;
        r.doc.set("mix", 0.5);
        r.hist.load(r.doc.blob());
        r.gc.step("mix", +1, false);
        CHECK(std::fabs(r.doc.get("mix") - 0.51) < 1e-12, "+1%%");
        r.gc.step("mix", -1, true);
        CHECK(std::fabs(r.doc.get("mix") - 0.509) < 1e-12, "-0.1%% fine");
        r.gc.step("input.gain", +1, false);
        CHECK(std::fabs(r.doc.get("input.gain") - 0.48) < 1e-12, "1%% of 48 dB span");
        r.gc.resetToFactory("mix");
        CHECK(r.doc.get("mix") == 1.0, "Home -> factory");
        r.gc.step("mix", +1, false);
        CHECK(r.doc.get("mix") == 1.0, "clamped at max");
        CHECK(!r.gc.step("mix", 0, false), "zero direction refused");
        r.gc.beginBias({0, 0});
        CHECK(!r.gc.step("mix", 1, false) && !r.gc.resetToFactory("mix"), "no discrete action during a gesture");
        r.gc.cancel();
    }
}
}  // namespace

int main() {
    testBias();
    testPinch();
    testRollbackUndoRate();
    testBrushMaterialNumeric();
    if (g_fail) { std::printf("uimodel gesture: %d FAILED\n", g_fail); return 1; }
    std::printf("uimodel gesture: all UX-01 checks passed\n");
    return 0;
}
