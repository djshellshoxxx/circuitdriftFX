// Temporary stub so the tree links; the panels owner replaces this file.
#include "ui/panels/Panels.h"

namespace cd::plugin::ui {
const char* panelTitle(PanelId id) noexcept {
    static const char* const titles[] = {"Presets", "A/B", "History", "Lifecycle", "Faults", "Blueprint",
                                         "Musical", "Capture", "Network", "Community", "Quality"};
    const int i = static_cast<int>(id);
    return i >= 0 && i < static_cast<int>(PanelId::Count) ? titles[i] : "";
}
namespace {
struct StubPanel final : Panel {
    void refresh(const StatusInfo&) override {}
    void settingsChanged(const UiSettings&) override {}
};
}  // namespace
std::unique_ptr<Panel> createPanel(PanelId, EditorBridge&) { return std::make_unique<StubPanel>(); }
}  // namespace cd::plugin::ui
