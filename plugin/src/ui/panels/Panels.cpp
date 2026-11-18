// Secondary panels factory (panels owner). Implementations live in the *Panels.cpp files.
#include "ui/panels/Panels.h"
#include "ui/panels/PanelCommon.h"

namespace cd::plugin::ui {

const char* panelTitle(PanelId id) noexcept {
    static const char* const titles[] = {"Presets", "A/B", "History", "Lifecycle", "Faults", "Blueprint",
                                         "Musical", "Capture", "Network", "Community", "Quality"};
    const int i = static_cast<int>(id);
    return i >= 0 && i < static_cast<int>(PanelId::Count) ? titles[i] : "";
}

std::unique_ptr<Panel> createPanel(PanelId id, EditorBridge& b) {
    switch (id) {
        case PanelId::Presets: return pc::makePresetsPanel(b);
        case PanelId::AB: return pc::makeABPanel(b);
        case PanelId::History: return pc::makeHistoryPanel(b);
        case PanelId::Lifecycle: return pc::makeLifecyclePanel(b);
        case PanelId::Faults: return pc::makeFaultsPanel(b);
        case PanelId::Blueprint: return pc::makeBlueprintPanel(b);
        case PanelId::Musical: return pc::makeMusicalPanel(b);
        case PanelId::Capture: return pc::makeCapturePanel(b);
        case PanelId::Network: return pc::makeNetworkPanel(b);
        case PanelId::Community: return pc::makeCommunityPanel(b);
        case PanelId::Quality: return pc::makeQualityPanel(b);
        case PanelId::Count: break;
    }
    return nullptr;
}

}  // namespace cd::plugin::ui
