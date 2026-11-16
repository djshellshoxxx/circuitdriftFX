// CircuitDrift uimodel: UX-02 Easy/Workbench shell state, factory quick scenes, T07 responsive
// layout model, theme tokens and keyboard/numeric accessibility map.
// Owner: cd_uimodel. Framework-independent, message thread only (allocates). No audio access:
// ShellModel changes only UI state (mode, scale, theme, plot tab, camera, viewport); resizing or
// switching view can never dirty a sonic preset or touch a ParamDocument/History.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "core/ParameterRegistry.h"
#include "state/History.h"
#include "uimodel/Document.h"

namespace cd::uimodel {

// ---------- UX-02 quick scenes (factory, versioned) ----------
struct FactoryScene {
    const char* id;
    const char* name;
    int version;                 // bump on any value change; projects keep scene id + version
    double age, sag, drift, warmth;
    bool analogNoise;
    const char* graphId;         // declared factory graph/seed/model versions
    std::uint32_t seed;
    const char* modelVersion;
};
inline constexpr int kSceneCount = 4;
const FactoryScene& factoryScene(int index) noexcept;   // clamped
int findScene(const std::string& id) noexcept;          // -1 if unknown

// Loads scene `index` into `doc` as ONE history transaction (macros, analogNoise, mix=100%,
// Auto-Gain on). Returns false on bad index or rejected commit.
bool loadScene(ParamDocument& doc, state::History& history, int index);
// True when any value a scene controls differs from scene `index` (the "Modified" flag).
bool diverged(const ParamDocument& doc, int index);

// ---------- UI state ----------
enum class ViewMode : std::uint8_t { Easy, Workbench };
enum class Theme : std::uint8_t { Dark, Light };
enum class PlotTab : std::uint8_t { Thermal, Rail, Phase };
struct Camera { double yaw = 0, pitch = 0, zoom = 1; };

inline constexpr double kMinViewportW = 750, kMinViewportH = 540, kBaseW = 1000, kBaseH = 720;
inline constexpr double kNarrowWidth = 900;

struct UiState {
    ViewMode mode = ViewMode::Easy;
    Theme theme = Theme::Dark;
    PlotTab plot = PlotTab::Thermal;
    Camera camera{};
    double scale = 1.0;            // 0.75..2.0 UI scale (not OS DPI)
    double viewportW = kBaseW, viewportH = kBaseH;   // logical
    bool inspectorDrawerOpen = false;
    int sceneIndex = -1;           // last loaded factory scene
    int sceneVersion = 0;
};

class ShellModel {
public:
    const UiState& state() const noexcept { return s_; }
    void setMode(ViewMode m) noexcept { s_.mode = m; }
    void setTheme(Theme t) noexcept { s_.theme = t; }
    void setPlot(PlotTab p) noexcept { s_.plot = p; }
    void setCamera(Camera c) noexcept;
    bool setScale(double s) noexcept;        // false (unchanged) outside 0.75..2 or non-finite
    // Host resize: below the 750x540 minimum (or non-finite) the request is refused and the
    // previous bounds are kept; content then reflows to the kept bounds.
    bool requestResize(double logicalW, double logicalH) noexcept;
    void setInspectorDrawer(bool open) noexcept { s_.inspectorDrawerOpen = open; }
    void noteSceneLoaded(int index) noexcept;
    // Modified flag for the loaded scene (false when no scene was loaded).
    bool modified(const ParamDocument& doc) const { return s_.sceneIndex >= 0 && diverged(doc, s_.sceneIndex); }
private:
    UiState s_;
};

// ---------- T07 layout model ----------
struct Rect { double x = 0, y = 0, w = 0, h = 0; };
struct LayoutItem {
    std::string id;
    Rect r;
    double minW = 32, minH = 32;     // required for text/value to be unclipped
    bool essential = true;
};
struct Layout {
    std::vector<LayoutItem> items;
    bool drawerMode = false;         // width < 900: inspector becomes a drawer, diagnostics tabs
    double vw = 0, vh = 0;
};
Layout computeLayout(ViewMode mode, double logicalW, double logicalH);   // clamps to the 750x540 minimum
// Empty string when valid; otherwise the first problem (clipping, overlap, below min size,
// missing essential control, macro order).
std::string validateLayout(const Layout& l, ViewMode mode);
struct PixelRect { int x = 0, y = 0, w = 0, h = 0; };
PixelRect toPixels(const Rect& r, double uiScale, double osDpiScale) noexcept;   // edge-rounded: no new overlap

inline constexpr std::array<double, 5> kUiScales = {0.75, 1.0, 1.25, 1.5, 2.0};
inline constexpr std::array<double, 5> kOsDpiScales = {1.0, 1.25, 1.5, 2.0, 3.0};

// ---------- themes ----------
struct ThemeTokens {
    const char* name;
    std::uint32_t background, surface, text, textMuted, outline, focus, accent;   // 0xRRGGBB
};
const ThemeTokens& themeTokens(Theme t) noexcept;
double contrastRatio(std::uint32_t rgbA, std::uint32_t rgbB) noexcept;   // WCAG 2.x

// ---------- accessibility ----------
struct A11yAction {
    std::string paramId;          // registry id or UI tool id
    std::string accessibleName;   // screen-reader control name
    std::string keyIncrease, keyDecrease, keyFineIncrease, keyFineDecrease, keyReset;
    bool numericEntry = false;
    bool sonic = true;
    int focusOrder = 0;
};
// One action per registry parameter (every one treated as sonic: the registry holds no
// non-sonic parameter) plus the pointer-tool equivalents (bias, supply, age brush, material).
std::vector<A11yAction> accessibilityMap();

}  // namespace cd::uimodel
