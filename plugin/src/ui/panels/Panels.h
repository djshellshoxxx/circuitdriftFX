// CircuitDrift plugin: secondary panels hosted by the editor's drawer/tab host.
// Contract (coordinator-owned): PanelId values, titles and createPanel() are stable; the panels
// owner implements them in plugin/src/ui/panels/*.cpp. Message thread only.
#pragma once
#include <memory>
#include "ui/EditorBridge.h"

namespace cd::plugin::ui {

enum class PanelId : int {
    Presets = 0,   // CD-F24 preset browser (factory + user, import/export, Modified)
    AB,            // CD-F24 A/B slots: edit slot, copy, swap, crossfade
    History,       // CD-F09 undo/redo list
    Lifecycle,     // CD-F18 / UX-03 hardware lifecycle scrubber and Restoration commit
    Faults,        // CD-F19 fault injector
    Blueprint,     // CD-F10 blueprint editor (validate / apply)
    Musical,       // CD-F12..F16 key, tempo, harmonics, transient, sidechain
    Capture,       // CD-F03 PNG/GIF/MP4 export
    Network,       // CD-F11 multi-instance network
    Community,     // CD-F04 community browser (offline/no service status)
    Quality,       // CD-F21/F25 quality, oversampling, latency, protection
    Count
};

const char* panelTitle(PanelId id) noexcept;

class Panel : public juce::Component {
public:
    ~Panel() override = default;
    // Called by the editor at UI rate with the latest status so panels need no own timers.
    virtual void refresh(const StatusInfo& status) = 0;
    virtual void settingsChanged(const UiSettings& settings) = 0;
};

std::unique_ptr<Panel> createPanel(PanelId id, EditorBridge& bridge);

}  // namespace cd::plugin::ui
