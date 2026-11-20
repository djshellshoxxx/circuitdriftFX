#include "ui/shell/ShellCommon.h"
#include "ui/workbench/GestureMath.h"

namespace cd::plugin::ui::shell {

juce::String formatValue(Fmt fmt, double v) {
    switch (fmt) {
        case Fmt::Percent: return juce::String(juce::roundToInt(v * 100.0)) + " %";
        case Fmt::Db: return (std::abs(v) < 0.05 ? juce::String("0.0") : juce::String(v, 1)) + " dB";
        case Fmt::Hz:
            if (v >= 1000.0) return juce::String(v / 1000.0, 1) + " kHz";
            return juce::String(v, v < 100.0 ? 1 : 0) + " Hz";
        case Fmt::Plain: break;
    }
    return juce::String(v, 2);
}

bool parseValue(Fmt fmt, const juce::String& text, double& out) {
    const char* unit = fmt == Fmt::Percent ? "%" : fmt == Fmt::Db ? "dB" : fmt == Fmt::Hz ? "Hz" : "";
    const auto parsed = gesture::parseQuantity(text.toStdString(), unit);
    if (!parsed.ok) return false;
    out = fmt == Fmt::Percent ? parsed.value / 100.0 : parsed.value;
    return true;
}

void drawPlate(juce::Graphics& g, juce::Rectangle<float> r, const Theme& t, bool raised, float corner) {
    const auto top = raised ? t.panelRaised : t.panel;
    const auto bottom = raised ? t.panel : t.inset.interpolatedWith(t.panel, 0.5f);
    g.setGradientFill(juce::ColourGradient(top, r.getX(), r.getY(), bottom, r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle(r, corner);
    g.setColour((t.dark ? juce::Colours::white : juce::Colours::white).withAlpha(t.dark ? 0.07f : 0.8f));
    g.drawRoundedRectangle(r.reduced(1.0f), corner - 1.0f, 1.0f);  // top-lit bevel
    g.setColour(t.dark ? juce::Colours::black.withAlpha(0.6f) : t.border.withAlpha(0.45f));
    g.drawRoundedRectangle(r, corner, 1.0f);
}

void drawEngraved(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, const juce::Font& font,
                  const Theme& t, juce::Justification j, juce::Colour colour) {
    g.setFont(font);
    g.setColour(t.dark ? juce::Colours::black.withAlpha(0.55f) : juce::Colours::white.withAlpha(0.9f));
    g.drawText(text, r.translated(0.0f, 1.0f), j, false);
    g.setColour(colour);
    g.drawText(text, r, j, false);
}

void drawScrew(juce::Graphics& g, juce::Point<float> c, float radius, const Theme& t, float angle) {
    juce::Rectangle<float> r(c.x - radius, c.y - radius, radius * 2, radius * 2);
    g.setGradientFill(juce::ColourGradient(t.panelRaised.brighter(t.dark ? 0.5f : 0.0f), r.getX(), r.getY(),
                                           t.inset, r.getRight(), r.getBottom(), true));
    g.fillEllipse(r);
    g.setColour(t.border.withAlpha(0.7f));
    g.drawEllipse(r, 0.8f);
    juce::Path slot;
    slot.addRectangle(-radius * 0.75f, -0.7f, radius * 1.5f, 1.4f);
    g.setColour(t.dark ? juce::Colours::black.withAlpha(0.8f) : t.border);
    g.fillPath(slot, juce::AffineTransform::rotation(angle).translated(c.x, c.y));
}

void drawFocusRing(juce::Graphics& g, juce::Rectangle<float> r, const Theme& t, float corner) {
    g.setColour(t.focus);
    g.drawRoundedRectangle(r.reduced(t.focusOutline * 0.5f), corner, t.focusOutline);
}

juce::Image makeBrushedBackground(int w, int h, const Theme& t) {
    w = std::max(w, 1);
    h = std::max(h, 1);
    juce::Image img(juce::Image::ARGB, w, h, true);
    juce::Graphics g(img);
    const auto lighter = t.background.interpolatedWith(t.dark ? juce::Colours::white : juce::Colours::white, t.dark ? 0.05f : 0.25f);
    g.setGradientFill(juce::ColourGradient(lighter, 0, 0, t.background, 0, (float) h, false));
    g.fillAll();
    juce::Random rng(0xC1D0A11);  // render-only deterministic RNG, unrelated to the audio RNG
    for (int y = 0; y < h; ++y) {
        const float a = rng.nextFloat() * (t.dark ? 0.045f : 0.05f);
        g.setColour(((y & 1) ? juce::Colours::white : juce::Colours::black).withAlpha(a));
        g.fillRect(0, y, w, 1);
    }
    const int streaks = (w * h) / 900;
    for (int i = 0; i < streaks; ++i) {
        const int x = rng.nextInt(w), y = rng.nextInt(h), len = 20 + rng.nextInt(std::max(40, w / 5));
        g.setColour((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha(0.02f + 0.02f * rng.nextFloat()));
        g.fillRect(x, y, len, 1);
    }
    return img;
}

}  // namespace cd::plugin::ui::shell
