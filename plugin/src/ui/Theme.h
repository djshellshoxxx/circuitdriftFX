// CircuitDrift plugin: shared design tokens (coordinator-owned contract; values may be tuned by
// the shell owner, names are stable). Dark brushed-metal and Light themes carry the same
// information hierarchy; text contrast >= 4.5:1 and interactive outlines >= 3:1 against `panel`.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace cd::plugin::ui {

struct Theme {
    bool dark = true;
    // Surfaces
    juce::Colour background, panel, panelRaised, inset, border;
    // Text
    juce::Colour text, textDim, textInverse;
    // Interaction
    juce::Colour accent, accentDim, focus, warning, danger, ok;
    // Data / analysis
    juce::Colour thermalCold, thermalNormal, thermalHot, voltageNeg, voltagePos, signal, grid;
    juce::Colour slotA, slotB;
    // PCB
    juce::Colour pcbSubstrate, pcbTrace, pcbSilk, pcbCopper;
    // Metrics (logical pixels at scale 1.0)
    float corner = 6.0f, outline = 1.5f, focusOutline = 2.0f;
    int gap = 8, padding = 12;
    float textSmall = 12.0f, textBody = 14.0f, textTitle = 18.0f, textValue = 13.0f;
};

inline const Theme& darkTheme() {
    static const Theme t = [] {
        Theme x;
        x.dark = true;
        x.background = juce::Colour(0xff15171a); x.panel = juce::Colour(0xff1f2226);
        x.panelRaised = juce::Colour(0xff2a2e33); x.inset = juce::Colour(0xff111316);
        x.border = juce::Colour(0xff8a929c);
        x.text = juce::Colour(0xffeef1f4); x.textDim = juce::Colour(0xffb4bcc6); x.textInverse = juce::Colour(0xff15171a);
        x.accent = juce::Colour(0xffe8a33d); x.accentDim = juce::Colour(0xff8f6526); x.focus = juce::Colour(0xff6cc4ff);
        x.warning = juce::Colour(0xfff0c24b); x.danger = juce::Colour(0xffff6b5e); x.ok = juce::Colour(0xff6fd38a);
        x.thermalCold = juce::Colour(0xff3d7bd9); x.thermalNormal = juce::Colour(0xffe8a33d); x.thermalHot = juce::Colour(0xffff4b3a);
        x.voltageNeg = juce::Colour(0xff4aa8ff); x.voltagePos = juce::Colour(0xffff8a3d); x.signal = juce::Colour(0xff7be3c6);
        x.grid = juce::Colour(0xff3a4048); x.slotA = juce::Colour(0xff6cc4ff); x.slotB = juce::Colour(0xffd58bff);
        x.pcbSubstrate = juce::Colour(0xff1d3b2a); x.pcbTrace = juce::Colour(0xffb88a3a); x.pcbSilk = juce::Colour(0xffe9ecef);
        x.pcbCopper = juce::Colour(0xffc58b45);
        return x;
    }();
    return t;
}

inline const Theme& lightTheme() {
    static const Theme t = [] {
        Theme x;
        x.dark = false;
        x.background = juce::Colour(0xffe9ecf0); x.panel = juce::Colour(0xfff7f8fa);
        x.panelRaised = juce::Colour(0xffffffff); x.inset = juce::Colour(0xffdfe3e8);
        x.border = juce::Colour(0xff5d6670);
        x.text = juce::Colour(0xff15181c); x.textDim = juce::Colour(0xff444c55); x.textInverse = juce::Colour(0xfff7f8fa);
        x.accent = juce::Colour(0xffa05f00); x.accentDim = juce::Colour(0xffd9b98a); x.focus = juce::Colour(0xff005fb8);
        x.warning = juce::Colour(0xff8a6200); x.danger = juce::Colour(0xffb3261e); x.ok = juce::Colour(0xff1e7a3a);
        x.thermalCold = juce::Colour(0xff1f5fbf); x.thermalNormal = juce::Colour(0xffb36b00); x.thermalHot = juce::Colour(0xffc62828);
        x.voltageNeg = juce::Colour(0xff1565c0); x.voltagePos = juce::Colour(0xffc2410c); x.signal = juce::Colour(0xff00796b);
        x.grid = juce::Colour(0xffc4cbd3); x.slotA = juce::Colour(0xff005fb8); x.slotB = juce::Colour(0xff7b1fa2);
        x.pcbSubstrate = juce::Colour(0xff2f6b48); x.pcbTrace = juce::Colour(0xffd9a441); x.pcbSilk = juce::Colour(0xfffafafa);
        x.pcbCopper = juce::Colour(0xffd08f45);
        return x;
    }();
    return t;
}

inline const Theme& themeFor(bool dark) { return dark ? darkTheme() : lightTheme(); }

}  // namespace cd::plugin::ui
