// CircuitDrift editor shell: shared constants, context and small drawing helpers.
// Owned by the UI shell agent (plugin/src/ui/shell/**). Message thread only.
#pragma once
#include <algorithm>
#include <cmath>
#include "ui/EditorBridge.h"
#include "ui/Theme.h"

namespace cd::plugin::ui::shell {

// ---- Constants (spec: 06-interface-inspector-export "Responsive vector shell", UX-02/07/10) ----
inline constexpr int kTooltipDelayMs = 450;      // hover or keyboard focus before a tooltip opens
inline constexpr int kBaseWidth = 1000, kBaseHeight = 720;
inline constexpr int kMinWidth = 750, kMinHeight = 540;
inline constexpr int kNarrowBelow = 900;         // logical width below which panels become a drawer
inline constexpr float kMinScale = 0.75f, kMaxScale = 2.0f;
inline constexpr int kStaleMs = 250;             // telemetry older than this gets the Stale badge
inline constexpr double kSphereSmoothingSeconds = 0.080;  // UX-07 visual smoothing

// Shared UI state handed to every shell widget (single instance owned by EditorCore).
struct ShellContext {
    UiSettings ui;
    bool pinTooltips = false;        // session-only option, not persisted
    const Theme& theme() const noexcept { return themeFor(ui.darkTheme); }
};

// ---- Contrast (WCAG 2.x relative luminance) ---------------------------------------------------
inline double channelLinear(float v) noexcept { return v <= 0.03928f ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
inline double relativeLuminance(juce::Colour c) noexcept {
    return 0.2126 * channelLinear(c.getFloatRed()) + 0.7152 * channelLinear(c.getFloatGreen()) +
           0.0722 * channelLinear(c.getFloatBlue());
}
inline double contrastRatio(juce::Colour a, juce::Colour b) noexcept {
    const double la = relativeLuminance(a), lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// ---- Fonts ---------------------------------------------------------------------------------------
inline juce::Font uiFont(float height, bool bold = false) {
    return juce::Font(juce::FontOptions(height).withStyle(bold ? "Bold" : "Regular"));
}
inline juce::Font monoFont(float height, bool bold = false) {
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height,
                                        bold ? juce::Font::bold : juce::Font::plain));
}

inline float textWidth(const juce::Font& f, const juce::String& s) {
    juce::GlyphArrangement ga;
    ga.addLineOfText(f, s, 0.0f, 0.0f);
    return ga.getBoundingBox(0, -1, true).getWidth();
}

// ---- Value formats for knobs/readouts ----------------------------------------------------------
enum class Fmt { Percent, Db, Hz, Plain };
juce::String formatValue(Fmt fmt, double v);
// Parses typed text ("45", "45 %", "-3 dB", "120Hz") into the plain value; false when no number.
bool parseValue(Fmt fmt, const juce::String& text, double& out);

// ---- Drawing helpers (vintage lab instrument look) -------------------------------------------
// Brushed-metal panel plate with bevel; `raised` plates are lighter.
void drawPlate(juce::Graphics& g, juce::Rectangle<float> r, const Theme& t, bool raised, float corner);
// Engraved text: a 1 px highlight below, then the text itself.
void drawEngraved(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, const juce::Font& font,
                  const Theme& t, juce::Justification j, juce::Colour colour);
// Slotted screw head.
void drawScrew(juce::Graphics& g, juce::Point<float> centre, float radius, const Theme& t, float angle);
// Keyboard focus outline (>= 3:1 against panel).
void drawFocusRing(juce::Graphics& g, juce::Rectangle<float> r, const Theme& t, float corner);

// Cached brushed-metal background; deterministic render-only noise (never the audio RNG).
juce::Image makeBrushedBackground(int w, int h, const Theme& t);

}  // namespace cd::plugin::ui::shell
