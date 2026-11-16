// CircuitDrift uimodel: shell state, factory scenes, layout, themes, accessibility map.
// Owner: cd_uimodel. Message thread only (allocates).
#include "uimodel/Shell.h"
#include <algorithm>
#include <cmath>

namespace cd::uimodel {

// ---------------- scenes ----------------
namespace {
constexpr FactoryScene kScenes[kSceneCount] = {
    {"clean-console", "Clean Console", 1, 0.0, 0.0, 0.0, 0.0, false, "builtin.synthetic-diode-clipper", 1u, "fixture-v1"},
    {"warm-tube-preamp", "Warm Tube Preamp", 1, 0.15, 0.2, 0.1, 0.55, false, "builtin.synthetic-diode-clipper", 2u, "fixture-v1"},
    {"pumping-bus-glue", "Pumping Bus Glue", 1, 0.1, 0.8, 0.1, 0.35, false, "builtin.synthetic-diode-clipper", 3u, "fixture-v1"},
    {"lofi-attic-gear", "Lo-Fi Attic Gear", 1, 0.85, 0.65, 0.6, 0.45, true, "builtin.synthetic-diode-clipper", 4u, "fixture-v1"},
};
struct SceneItem { const char* id; double v; };
std::array<SceneItem, 7> sceneValues(const FactoryScene& s) noexcept {
    return {{{"macro.age", s.age}, {"macro.sag", s.sag}, {"macro.drift", s.drift}, {"macro.warmth", s.warmth},
             {"analogNoise.enabled", s.analogNoise ? 1.0 : 0.0}, {"mix", 1.0}, {"gainMatch.enabled", 1.0}}};
}
}  // namespace

const FactoryScene& factoryScene(int i) noexcept { return kScenes[std::clamp(i, 0, kSceneCount - 1)]; }

int findScene(const std::string& id) noexcept {
    for (int i = 0; i < kSceneCount; ++i) if (id == kScenes[i].id) return i;
    return -1;
}

bool loadScene(ParamDocument& doc, state::History& history, int index) {
    if (index < 0 || index >= kSceneCount) return false;
    const auto vals = sceneValues(kScenes[index]);
    for (const auto& v : vals) if (!doc.find(v.id)) return false;
    const std::string before = doc.serialize();
    std::vector<std::string> ids;
    for (const auto& v : vals) { doc.set(v.id, v.v); ids.push_back(v.id); }
    const auto r = history.commit(std::string("Load scene: ") + kScenes[index].name, state::TxOrigin::SceneLoad, ids, doc.blob());
    if (r == state::CommitResult::Rejected) { doc.restore(before); return false; }
    return true;
}

bool diverged(const ParamDocument& doc, int index) {
    if (index < 0 || index >= kSceneCount) return false;
    for (const auto& v : sceneValues(kScenes[index])) if (doc.get(v.id) != v.v) return true;
    return false;
}

// ---------------- shell ----------------
void ShellModel::setCamera(Camera c) noexcept {
    if (std::isfinite(c.yaw) && std::isfinite(c.pitch) && std::isfinite(c.zoom) && c.zoom > 0) s_.camera = c;
}
bool ShellModel::setScale(double s) noexcept {
    if (!std::isfinite(s) || s < 0.75 || s > 2.0) return false;
    s_.scale = s;
    return true;
}
bool ShellModel::requestResize(double w, double h) noexcept {
    if (!std::isfinite(w) || !std::isfinite(h) || w < kMinViewportW || h < kMinViewportH) return false;
    s_.viewportW = w;
    s_.viewportH = h;
    return true;
}
void ShellModel::noteSceneLoaded(int index) noexcept {
    s_.sceneIndex = index;
    s_.sceneVersion = (index >= 0 && index < kSceneCount) ? kScenes[index].version : 0;
}

// ---------------- layout ----------------
namespace {
constexpr double kG = 16, kGap = 8, kHeader = 48, kRow = 40, kMacroH = 96, kTabH = 32, kPlotMinH = 100;

void put(Layout& l, const char* id, Rect r, double minW, double minH, bool essential = true) {
    l.items.push_back({id, r, minW, minH, essential});
}

// Left-to-right packing of fixed-width items at y.
double packRow(Layout& l, double x, double y, double h, std::initializer_list<std::pair<const char*, double>> items) {
    for (const auto& it : items) {
        put(l, it.first, {x, y, it.second, h}, it.second, 32);
        x += it.second + kGap;
    }
    return x;
}

double utilityBar(Layout& l, double vw, double vh) {   // returns top y of the bar
    static const char* ids[] = {"util.input", "util.autoGain", "util.hpfEnable", "util.hpfCutoff", "util.mix",
                                "util.output", "util.softClip", "util.hq", "util.bypass"};
    constexpr int n = 9;
    constexpr double minW = 72;
    const double avail = vw - 2 * kG;
    const int perRow = std::max(1, int(std::floor((avail + kGap) / (minW + kGap))));
    const int rows = (n + perRow - 1) / perRow;
    const double top = vh - kGap - rows * kRow - (rows - 1) * kGap;
    for (int i = 0; i < n; ++i) {
        const int row = i / perRow, col = i % perRow;
        const int inRow = std::min(perRow, n - row * perRow);
        const double w = (avail - (inRow - 1) * kGap) / inRow;
        put(l, ids[i], {kG + col * (w + kGap), top + row * (kRow + kGap), w, kRow}, minW, 32);
    }
    return top;
}

double macroRow(Layout& l, double vw, double yBottom) {   // returns top y
    static const char* ids[] = {"macro.age", "macro.sag", "macro.drift", "macro.warmth"};
    const double avail = vw - 2 * kG;
    const double w = (avail - 3 * kGap) / 4;
    const double top = yBottom - kMacroH;
    for (int i = 0; i < 4; ++i) put(l, ids[i], {kG + i * (w + kGap), top, w, kMacroH}, 120, 80);
    return top;
}
}  // namespace

Layout computeLayout(ViewMode mode, double w, double h) {
    Layout l;
    l.vw = std::isfinite(w) ? std::max(w, kMinViewportW) : kBaseW;
    l.vh = std::isfinite(h) ? std::max(h, kMinViewportH) : kBaseH;
    l.drawerMode = l.vw < kNarrowWidth;
    const double vw = l.vw, vh = l.vh;
    const double utilTop = utilityBar(l, vw, vh);
    const double macroTop = macroRow(l, vw, utilTop - kGap);
    if (mode == ViewMode::Easy) {
        const double hx = packRow(l, kG, kGap, kHeader, {{"header.product", 140}, {"header.preset", 180}, {"header.abA", 64}, {"header.abB", 64}});
        (void)hx;
        put(l, "header.workbench", {vw - kG - 120, kGap, 120, kHeader}, 120, 32);
        const double top = kGap + kHeader + kGap, bottom = macroTop - kGap;
        const double side = std::min(bottom - top, vw - 2 * kG);
        put(l, "sphere", {(vw - side) / 2, top + (bottom - top - side) / 2, side, side}, 120, 120);
    } else {
        packRow(l, kG, kGap, kHeader, {{"header.lifecycle", 200}, {"header.scenario", 140}, {"header.abA", 64}, {"header.abB", 64}});
        put(l, "header.returnEasy", {vw - kG - 140, kGap, 140, kHeader}, 140, 32);
        const double tabY = macroTop - kGap - kTabH;
        const double plotH = kPlotMinH;
        const double plotY = tabY - kGap - plotH;
        for (int i = 0; i < 3; ++i) {
            static const char* ids[] = {"tab.thermal", "tab.rail", "tab.phase"};
            put(l, ids[i], {kG + i * (120 + kGap), tabY, 120, kTabH}, 120, 32);
        }
        put(l, "plot", {kG, plotY, vw - 2 * kG, plotH}, 200, kPlotMinH, false);
        const double top = kGap + kHeader + kGap, bottom = plotY - kGap;
        if (l.drawerMode) {
            put(l, "inspector.drawerToggle", {vw - kG - 140, tabY, 140, kTabH}, 140, 32);
            put(l, "pcb", {kG, top, vw - 2 * kG, bottom - top}, 200, 120, false);
        } else {
            const double insW = 260;
            put(l, "pcb", {kG, top, vw - 2 * kG - insW - kGap, bottom - top}, 200, 120, false);
            put(l, "inspector", {vw - kG - insW, top, insW, bottom - top}, 200, 120, false);
        }
    }
    return l;
}

std::string validateLayout(const Layout& l, ViewMode mode) {
    constexpr double eps = 1e-9;
    for (const auto& it : l.items) {
        if (!(it.r.w >= it.minW - eps) || !(it.r.h >= it.minH - eps)) return it.id + ": below minimum size (clipped)";
        if (it.r.x < -eps || it.r.y < -eps || it.r.x + it.r.w > l.vw + eps || it.r.y + it.r.h > l.vh + eps) return it.id + ": outside viewport";
        if (it.essential && (it.r.w < 32 - eps || it.r.h < 32 - eps)) return it.id + ": hit target < 32 logical px";
    }
    for (std::size_t i = 0; i < l.items.size(); ++i)
        for (std::size_t j = i + 1; j < l.items.size(); ++j) {
            const Rect &a = l.items[i].r, &b = l.items[j].r;
            if (a.x < b.x + b.w - eps && b.x < a.x + a.w - eps && a.y < b.y + b.h - eps && b.y < a.y + a.h - eps)
                return l.items[i].id + " overlaps " + l.items[j].id;
        }
    auto find = [&](const char* id) -> const LayoutItem* {
        for (const auto& it : l.items) if (it.id == id) return &it;
        return nullptr;
    };
    std::vector<const char*> must = {"macro.age", "macro.sag", "macro.drift", "macro.warmth", "util.input", "util.autoGain",
                                     "util.hpfEnable", "util.hpfCutoff", "util.mix", "util.output", "util.softClip", "util.hq",
                                     "util.bypass", "header.abA", "header.abB"};
    must.push_back(mode == ViewMode::Easy ? "sphere" : "pcb");
    must.push_back(mode == ViewMode::Easy ? "header.workbench" : "header.returnEasy");
    for (const char* m : must) if (!find(m)) return std::string("missing essential control ") + m;
    if (!(find("macro.age")->r.x < find("macro.sag")->r.x && find("macro.sag")->r.x < find("macro.drift")->r.x &&
          find("macro.drift")->r.x < find("macro.warmth")->r.x))
        return "macro order must be Age, Sag, Drift, Warmth";
    if (mode == ViewMode::Workbench && l.drawerMode && !find("inspector.drawerToggle")) return "narrow inspector needs a drawer toggle";
    return {};
}

PixelRect toPixels(const Rect& r, double uiScale, double dpi) noexcept {
    const double k = uiScale * dpi;
    const int x0 = int(std::lround(r.x * k)), y0 = int(std::lround(r.y * k));
    const int x1 = int(std::lround((r.x + r.w) * k)), y1 = int(std::lround((r.y + r.h) * k));
    return {x0, y0, x1 - x0, y1 - y0};
}

// ---------------- themes ----------------
const ThemeTokens& themeTokens(Theme t) noexcept {
    static const ThemeTokens dark = {"dark-brushed-metal", 0x14161A, 0x1F2329, 0xE6E8EB, 0xA9B0BA, 0x8A93A0, 0x4FC3F7, 0xFFB347};
    static const ThemeTokens light = {"light", 0xF2F3F5, 0xFFFFFF, 0x1B1E23, 0x4A525C, 0x6B7480, 0x005FCC, 0x8A4B00};
    return t == Theme::Dark ? dark : light;
}

double contrastRatio(std::uint32_t a, std::uint32_t b) noexcept {
    auto lum = [](std::uint32_t c) {
        auto ch = [](double v) { v /= 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch((c >> 16) & 0xFF) + 0.7152 * ch((c >> 8) & 0xFF) + 0.0722 * ch(c & 0xFF);
    };
    const double la = lum(a), lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// ---------------- accessibility ----------------
std::vector<A11yAction> accessibilityMap() {
    std::vector<A11yAction> v;
    int order = 0;
    for (const auto& p : core::kParams) {
        A11yAction a;
        a.paramId = p.id;
        a.accessibleName = std::string(p.id) + " (" + p.unit + ")";
        a.numericEntry = true;
        a.focusOrder = order++;
        a.keyReset = "Home";
        if (p.kind == core::ParamKind::Bool) {
            a.keyIncrease = a.keyDecrease = "Space";
            a.keyFineIncrease = a.keyFineDecrease = "Shift+Space";
        } else {
            a.keyIncrease = "ArrowUp"; a.keyDecrease = "ArrowDown";
            a.keyFineIncrease = "Shift+ArrowUp"; a.keyFineDecrease = "Shift+ArrowDown";
        }
        v.push_back(std::move(a));
    }
    for (const char* tool : {"tube.bias", "supply.capacitance", "age.brush", "material.select"}) {
        A11yAction a;
        a.paramId = tool;
        a.accessibleName = std::string(tool) + " (keyboard equivalent of pointer tool)";
        a.keyIncrease = "ArrowUp"; a.keyDecrease = "ArrowDown";
        a.keyFineIncrease = "Shift+ArrowUp"; a.keyFineDecrease = "Shift+ArrowDown";
        a.keyReset = "Home";
        a.numericEntry = true;
        a.focusOrder = order++;
        v.push_back(std::move(a));
    }
    return v;
}

}  // namespace cd::uimodel
