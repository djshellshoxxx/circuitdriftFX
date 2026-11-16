// Tests: UX-02 Easy/Workbench + factory scenes, T07 layout/theme/accessibility (view-model only).
// Engine audio is rendered with SYNTHETIC circuits; no physical validation is claimed.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "runtime/Engine.h"
#include "uimodel/Gesture.h"
#include "uimodel/Shell.h"

namespace {
using namespace cd;
using namespace cd::uimodel;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d [%s]: ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

struct NullHost : EditHost {
    void beginEdit(const std::string&) override {}
    void performEdit(const std::string&, double) override {}
    void endEdit(const std::string&) override {}
};

void testScenes() {
    struct Row { const char* name; double a, s, d, w; bool noise; };
    const Row rows[kSceneCount] = {{"Clean Console", 0, 0, 0, 0, false}, {"Warm Tube Preamp", 0.15, 0.2, 0.1, 0.55, false},
                                   {"Pumping Bus Glue", 0.1, 0.8, 0.1, 0.35, false}, {"Lo-Fi Attic Gear", 0.85, 0.65, 0.6, 0.45, true}};
    for (int i = 0; i < kSceneCount; ++i) {
        ParamDocument doc = ParamDocument::fromRegistry();
        state::History h;
        h.load(doc.blob());
        CHECK(loadScene(doc, h, i), "load %d", i);
        CHECK(std::string(factoryScene(i).name) == rows[i].name, "name");
        CHECK(doc.get("macro.age") == rows[i].a && doc.get("macro.sag") == rows[i].s && doc.get("macro.drift") == rows[i].d &&
                  doc.get("macro.warmth") == rows[i].w, "exact macro values scene %d", i);
        CHECK(doc.get("analogNoise.enabled") == (rows[i].noise ? 1.0 : 0.0), "noise");
        CHECK(doc.get("mix") == 1.0 && doc.get("gainMatch.enabled") == 1.0, "100%% wet + Auto-Gain on");
        CHECK(h.undoDepth() <= 1, "loading is one transaction (%zu)", h.undoDepth());
        CHECK(factoryScene(i).version == 1 && findScene(factoryScene(i).id) == i, "versioned scene id");
        CHECK(!diverged(doc, i), "not modified right after load");
        doc.set("macro.sag", 0.123);
        CHECK(diverged(doc, i), "Modified when diverged");
    }
    CHECK(!loadScene(*std::make_unique<ParamDocument>(), *std::make_unique<state::History>(), 0), "empty doc refused");
    CHECK(findScene("nope") == -1 && !diverged(ParamDocument::fromRegistry(), 9), "unknown scene");
    // Workbench edit marks Modified; undo of the edit clears it.
    ParamDocument doc = ParamDocument::fromRegistry();
    state::History h;
    h.load(doc.blob());
    loadScene(doc, h, 1);
    ShellModel sh;
    sh.noteSceneLoaded(1);
    CHECK(!sh.modified(doc), "clean");
    NullHost nh;
    std::int64_t t = 0;
    GestureController gc(doc, h, nh, [&] { return t; });
    gc.enterNumeric("macro.age", "0.5");
    CHECK(sh.modified(doc), "Workbench edit -> Modified");
    const auto prev = h.undo();
    doc.restore(*prev);
    CHECK(!sh.modified(doc), "undo -> not Modified");
}

using Buf = std::vector<double>;
Buf render(bool toggleViews, ShellModel* shell, ParamDocument* doc, const std::string* docBefore) {
    runtime::EngineConfig c;
    c.sampleRate = 48000; c.maxBlock = 256; c.channels = 1;
    c.initialMacros = {0.15, 0.2, 0.1, 0.55};
    runtime::Engine eng;
    std::string err;
    if (!eng.prepare(c, err)) { std::printf("prepare failed: %s\n", err.c_str()); ++g_fail; return {}; }
    Buf out;
    for (int blk = 0; blk < 24; ++blk) {
        if (toggleViews) {   // UI-only actions between blocks
            shell->setMode(blk % 2 ? ViewMode::Easy : ViewMode::Workbench);
            shell->setTheme(blk % 3 ? Theme::Light : Theme::Dark);
            shell->setScale(kUiScales[std::size_t(blk) % kUiScales.size()]);
            shell->requestResize(750 + 10.0 * blk, 540 + 5.0 * blk);
            shell->setPlot(PlotTab(blk % 3));
            shell->setCamera({0.1 * blk, 0.2, 1.0 + 0.01 * blk});
            shell->setInspectorDrawer(blk % 2 == 0);
            CHECK(doc->serialize() == *docBefore, "document untouched by view changes");
        }
        std::vector<double> in(256), o(256);
        for (int i = 0; i < 256; ++i) in[std::size_t(i)] = 0.4 * std::sin(2 * M_PI * 440.0 * double(blk * 256 + i) / 48000.0);
        const double* ip[1] = {in.data()};
        double* op[1] = {o.data()};
        runtime::ProcessBlock b;
        b.ctx = {48000, std::int64_t(blk) * 256, 0, 256, false};
        b.buses = {1, ip, op};
        eng.process(b);
        out.insert(out.end(), o.begin(), o.end());
    }
    return out;
}

void testModeBitIdentity() {
    ParamDocument doc = ParamDocument::fromRegistry();
    const std::string before = doc.serialize();
    state::History h;
    h.load(doc.blob());
    ShellModel shell;
    const Buf a = render(false, nullptr, nullptr, nullptr);
    const Buf b = render(true, &shell, &doc, &before);
    CHECK(!a.empty() && a.size() == b.size(), "sizes");
    bool same = a.size() == b.size();
    double peak = 0;
    for (std::size_t i = 0; same && i < a.size(); ++i) {
        same = std::bit_cast<std::uint64_t>(a[i]) == std::bit_cast<std::uint64_t>(b[i]);
        peak = std::fmax(peak, std::fabs(a[i]));
    }
    CHECK(same, "UX-02: render bit-identical with view-mode/resize/theme churn");
    CHECK(peak > 0.01, "fixture is not silent (peak %g)", peak);
    CHECK(h.undoDepth() == 0, "view changes never create history");
}

void testPersistenceAcrossResize() {
    ShellModel sh;
    sh.setMode(ViewMode::Workbench);
    sh.setTheme(Theme::Light);
    sh.setPlot(PlotTab::Phase);
    sh.setCamera({0.5, 0.25, 2.0});
    CHECK(sh.setScale(1.5), "scale");
    CHECK(!sh.setScale(2.5) && !sh.setScale(0.5) && !sh.setScale(std::nan("")), "scale bounds");
    ParamDocument doc = ParamDocument::fromRegistry();
    state::History h;
    h.load(doc.blob());
    loadScene(doc, h, 2);
    sh.noteSceneLoaded(2);
    for (double w : {750.0, 899.0, 900.0, 1000.0, 1600.0, 800.0}) {
        CHECK(sh.requestResize(w, 560), "resize %g", w);
        CHECK(sh.state().mode == ViewMode::Workbench && sh.state().theme == Theme::Light && sh.state().plot == PlotTab::Phase &&
                  sh.state().scale == 1.5 && sh.state().camera.zoom == 2.0, "UI settings persist through resize");
        CHECK(!sh.modified(doc), "resize never dirties a preset");
    }
    CHECK(!sh.requestResize(700, 540) && !sh.requestResize(800, 500) && !sh.requestResize(std::nan(""), 600), "refused");
    CHECK(sh.state().viewportW == 800 && sh.state().viewportH == 560, "refusal keeps prior bounds");
    const Layout l = computeLayout(ViewMode::Easy, 100, 100);   // reflows to the minimum
    CHECK(l.vw == 750 && l.vh == 540 && validateLayout(l, ViewMode::Easy).empty(), "fit to minimum");
}

void testLayout() {
    const double sizes[][2] = {{750, 540}, {751, 541}, {820, 600}, {899, 700}, {900, 540}, {1000, 720}, {1280, 800}, {1920, 1080}, {3000, 2000}};
    int checked = 0;
    for (ViewMode m : {ViewMode::Easy, ViewMode::Workbench})
        for (Theme th : {Theme::Dark, Theme::Light})
            for (double sc : kUiScales)
                for (double dpi : kOsDpiScales)
                    for (const auto& s : sizes) {
                        // Window pixel size = logical viewport * scale * dpi; logical content is scale-independent.
                        const Layout l = computeLayout(m, s[0], s[1]);
                        const std::string why = validateLayout(l, m);
                        CHECK(why.empty(), "mode %d theme %d scale %g dpi %g %gx%g: %s", int(m), int(th), sc, dpi, s[0], s[1], why.c_str());
                        CHECK(l.drawerMode == (s[0] < 900), "drawer mode");
                        std::vector<PixelRect> px;
                        for (const auto& it : l.items) {
                            const PixelRect p = toPixels(it.r, sc, dpi);
                            px.push_back(p);
                            CHECK(p.x >= 0 && p.y >= 0 && p.x + p.w <= int(std::lround(l.vw * sc * dpi)) &&
                                      p.y + p.h <= int(std::lround(l.vh * sc * dpi)), "%s inside window in pixels", it.id.c_str());
                            if (it.essential) CHECK(p.w >= int(32 * sc * dpi) - 1 && p.h >= int(32 * sc * dpi) - 1, "%s hit target in px", it.id.c_str());
                        }
                        for (std::size_t i = 0; i < px.size(); ++i)
                            for (std::size_t j = i + 1; j < px.size(); ++j)
                                CHECK(!(px[i].x < px[j].x + px[j].w && px[j].x < px[i].x + px[i].w && px[i].y < px[j].y + px[j].h &&
                                        px[j].y < px[i].y + px[i].h), "pixel overlap %s/%s", l.items[i].id.c_str(), l.items[j].id.c_str());
                        ++checked;
                    }
    std::printf("uimodel T07: %d layout configurations (modes x themes x 5 scales x 5 DPI x 9 viewports)\n", checked);
    // Bypass/output/A-B always reachable even at the minimum, and the macros keep canonical order (validateLayout).
    const Layout narrow = computeLayout(ViewMode::Workbench, 750, 540);
    bool toggle = false;
    for (const auto& it : narrow.items) toggle = toggle || it.id == "inspector.drawerToggle";
    CHECK(toggle, "narrow inspector is a drawer");
    // Negative control: a broken layout must be detected.
    Layout bad = computeLayout(ViewMode::Easy, 1000, 720);
    bad.items[0].r.w = 10;
    CHECK(!validateLayout(bad, ViewMode::Easy).empty(), "clipping detected");
    bad = computeLayout(ViewMode::Easy, 1000, 720);
    bad.items[1].r = bad.items[2].r;
    CHECK(!validateLayout(bad, ViewMode::Easy).empty(), "overlap detected");
    bad = computeLayout(ViewMode::Easy, 1000, 720);
    bad.items.pop_back();
    bad.items.erase(bad.items.begin() + 3);
    CHECK(!validateLayout(bad, ViewMode::Easy).empty(), "missing control detected");
    bad = computeLayout(ViewMode::Easy, 1000, 720);
    bad.items[0].r.x = -5;
    CHECK(!validateLayout(bad, ViewMode::Easy).empty(), "outside viewport detected");
}

void testThemes() {
    for (Theme t : {Theme::Dark, Theme::Light}) {
        const ThemeTokens& k = themeTokens(t);
        CHECK(contrastRatio(k.text, k.background) >= 4.5, "%s text/bg %.2f", k.name, contrastRatio(k.text, k.background));
        CHECK(contrastRatio(k.text, k.surface) >= 4.5, "%s text/surface", k.name);
        CHECK(contrastRatio(k.textMuted, k.background) >= 4.5, "%s muted/bg %.2f", k.name, contrastRatio(k.textMuted, k.background));
        CHECK(contrastRatio(k.outline, k.background) >= 3.0, "%s outline/bg %.2f", k.name, contrastRatio(k.outline, k.background));
        CHECK(contrastRatio(k.focus, k.background) >= 3.0, "%s focus/bg %.2f", k.name, contrastRatio(k.focus, k.background));
        CHECK(contrastRatio(k.accent, k.background) >= 3.0, "%s accent/bg %.2f", k.name, contrastRatio(k.accent, k.background));
    }
    CHECK(std::fabs(contrastRatio(0x000000, 0xFFFFFF) - 21.0) < 1e-9, "WCAG reference 21:1");
    CHECK(std::fabs(contrastRatio(0x777777, 0x777777) - 1.0) < 1e-12, "identical = 1");
}

void testAccessibility() {
    const auto map = accessibilityMap();
    CHECK(map.size() == core::kParamCount + 4, "all registry params + tools (%zu)", map.size());
    ParamDocument doc = ParamDocument::fromRegistry();
    state::History h;
    h.load(doc.blob());
    NullHost nh;
    std::int64_t t = 0;
    GestureController gc(doc, h, nh, [&] { return t; });
    std::size_t covered = 0, automatable = 0;
    for (const auto& p : core::kParams) {
        const A11yAction* a = nullptr;
        for (const auto& x : map) if (x.paramId == p.id) a = &x;
        CHECK(a != nullptr, "%s has an accessibility entry", p.id);
        if (!a) continue;
        ++covered;
        automatable += p.automatable;
        CHECK(a->numericEntry && a->sonic && !a->accessibleName.empty(), "%s numeric + name", p.id);
        CHECK(!a->keyIncrease.empty() && !a->keyDecrease.empty() && !a->keyReset.empty(), "%s keyboard actions", p.id);
        // The numeric alternative really works for every parameter: type the default and a mid value.
        char text[64];
        std::snprintf(text, sizeof text, "%.17g", p.max);
        std::string err;
        CHECK(gc.enterNumeric(p.id, text, &err), "%s numeric entry of max %s: %s", p.id, text, err.c_str());
        CHECK(doc.get(p.id) == p.max, "%s applied", p.id);
        // Keyboard step and Home work and stay in range.
        gc.step(p.id, -1, false);
        gc.step(p.id, +1, true);
        CHECK(doc.get(p.id) >= p.min && doc.get(p.id) <= p.max, "%s step in range", p.id);
        gc.resetToFactory(p.id);
        CHECK(doc.get(p.id) == p.def, "%s Home -> factory", p.id);
        // Out-of-range text is refused without changing state.
        std::snprintf(text, sizeof text, "%.17g", p.max + 1000.0);
        const double before = doc.get(p.id);
        CHECK(!gc.enterNumeric(p.id, text, &err) && doc.get(p.id) == before, "%s out-of-range refused", p.id);
    }
    CHECK(covered == core::kParamCount && automatable > 0, "covered %zu automatable %zu", covered, automatable);
    for (std::size_t i = 0; i < map.size(); ++i) {
        CHECK(map[i].focusOrder == int(i), "focus order is a total order");
        for (std::size_t j = i + 1; j < map.size(); ++j) CHECK(map[i].paramId != map[j].paramId, "unique ids");
    }
    for (const char* tool : {"tube.bias", "supply.capacitance", "age.brush", "material.select"}) {
        bool f = false;
        for (const auto& x : map) f = f || x.paramId == tool;
        CHECK(f, "pointer tool %s has keyboard equivalent", tool);
    }
}
}  // namespace

int main() {
    testScenes();
    testModeBitIdentity();
    testPersistenceAcrossResize();
    testLayout();
    testThemes();
    testAccessibility();
    if (g_fail) { std::printf("uimodel shell: %d FAILED\n", g_fail); return 1; }
    std::printf("uimodel shell: all UX-02/T07 checks passed\n");
    return 0;
}
