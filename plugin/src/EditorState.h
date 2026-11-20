// Versioned editor preferences in the host state. All work is non-audio.
// Optional root attributes keep legacy parameter-only state readable.
#pragma once
#include "ui/EditorBridge.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace cd::plugin {

inline bool sameEditorSettings(const UiSettings& a, const UiSettings& b) noexcept {
    return a.scale == b.scale && a.darkTheme == b.darkTheme && a.view == b.view &&
           a.educational == b.educational && a.reduceMotion == b.reduceMotion &&
           a.lowFrameRate == b.lowFrameRate && a.overlay == b.overlay && a.xray == b.xray &&
           a.pcbView == b.pcbView && a.lastWidth == b.lastWidth && a.lastHeight == b.lastHeight;
}

inline UiSettings boundedEditorSettings(UiSettings value) noexcept {
    value.scale = std::isfinite(value.scale) ? std::clamp(value.scale, 0.75f, 2.0f) : 1.0f;
    if (value.view != EditorView::Easy && value.view != EditorView::Workbench) value.view = EditorView::Easy;
    value.overlay = std::clamp(value.overlay, 0, 3);
    value.pcbView = std::clamp(value.pcbView, 0, 2);
    value.lastWidth = std::clamp(value.lastWidth, 750, 4096);
    value.lastHeight = std::clamp(value.lastHeight, 540, 4096);
    return value;
}

inline void writeEditorSettings(juce::XmlElement& xml, const UiSettings& value) {
    xml.setAttribute("uiVersion", "1");
    xml.setAttribute("uiScale", static_cast<double>(value.scale));
    xml.setAttribute("uiDarkTheme", value.darkTheme ? 1 : 0);
    xml.setAttribute("uiView", static_cast<int>(value.view));
    xml.setAttribute("uiEducational", value.educational ? 1 : 0);
    xml.setAttribute("uiReduceMotion", value.reduceMotion ? 1 : 0);
    xml.setAttribute("uiLowFrameRate", value.lowFrameRate ? 1 : 0);
    xml.setAttribute("uiOverlay", value.overlay);
    xml.setAttribute("uiXray", value.xray ? 1 : 0);
    xml.setAttribute("uiPcbView", value.pcbView);
    xml.setAttribute("uiWidth", value.lastWidth);
    xml.setAttribute("uiHeight", value.lastHeight);
}

inline bool readEditorSettings(const juce::XmlElement& xml, UiSettings& out) {
    UiSettings value;
    if (!xml.hasAttribute("uiVersion")) { out = value; return true; }
    if (xml.getStringAttribute("uiVersion") != "1") return false;
    const auto number = [&](const char* name, auto& destination, auto minimum, auto maximum) {
        const auto encoded = xml.getStringAttribute(name).toStdString();
        const auto result = std::from_chars(encoded.data(), encoded.data() + encoded.size(), destination);
        return result.ec == std::errc{} && result.ptr == encoded.data() + encoded.size() &&
               std::isfinite(static_cast<double>(destination)) && destination >= minimum && destination <= maximum;
    };
    const auto boolean = [&](const char* name, bool& destination) {
        int parsed = 0;
        if (!number(name, parsed, 0, 1)) return false;
        destination = parsed != 0;
        return true;
    };
    int view = 0;
    if (!number("uiScale", value.scale, 0.75f, 2.0f) ||
        !boolean("uiDarkTheme", value.darkTheme) || !number("uiView", view, 0, 1) ||
        !boolean("uiEducational", value.educational) || !boolean("uiReduceMotion", value.reduceMotion) ||
        !boolean("uiLowFrameRate", value.lowFrameRate) || !number("uiOverlay", value.overlay, 0, 3) ||
        !boolean("uiXray", value.xray) || !number("uiPcbView", value.pcbView, 0, 2) ||
        !number("uiWidth", value.lastWidth, 750, 4096) || !number("uiHeight", value.lastHeight, 540, 4096))
        return false;
    value.view = static_cast<EditorView>(view);
    out = value;
    return true;
}

}  // namespace cd::plugin
