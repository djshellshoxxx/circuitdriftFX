// Preset selection must follow stable IDs as a visible catalog changes.
// Uses the production panel with a tracking bridge; no file chooser or disk I/O.
#pragma once
#include "FakeBridge.h"
#include "ui/panels/PanelCommon.h"
#include <algorithm>
#include <cstdio>
#include <memory>

namespace cd::plugin::test::preset_selection_regressions {

class TrackingBridge final : public FakeBridge {
public:
    std::vector<PresetEntry> catalog{
        {"user:amber", "Amber", "User", "", false},
        {"user:blue", "Blue", "User", "", false},
        {"factory:clean", "Clean", "Factory", "", true},
        {"factory:warm", "Warm", "Factory", "", true}};
    juce::StringArray loaded;
    juce::String catalogMessage;

    std::vector<PresetEntry> listPresets(const juce::String& query) override {
        std::vector<PresetEntry> result;
        for (const auto& entry : catalog)
            if (query.isEmpty() || entry.name.containsIgnoreCase(query)) result.push_back(entry);
        return result;
    }
    juce::String presetCatalogMessage() const override { return catalogMessage; }
    bool loadPreset(const juce::String& id, int, juce::String& error) override {
        loaded.add(id);
        error.clear();
        return true;
    }
};

template <class T>
T* findControl(juce::Component& root, const juce::String& title) {
    for (auto* child : root.getChildren()) {
        if (child->getTitle() == title)
            if (auto* control = dynamic_cast<T*>(child)) return control;
        if (auto* control = findControl<T>(*child, title)) return control;
    }
    return nullptr;
}

inline juce::MouseEvent doubleClick(juce::Component& component) {
    const juce::Point<float> position{2.0f, 2.0f};
    const juce::Time time;
    return {juce::Desktop::getInstance().getMainMouseSource(), position,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component,
            time, position, time, 2, false};
}

inline int run() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::printf("FAIL preset selection: %s\n", message); }
    };
    auto bridge = std::make_unique<TrackingBridge>();
    bridge->catalogMessage = "Only part of the preset catalog could be scanned.";
    auto panel = ui::createPanel(ui::PanelId::Presets, *bridge);
    panel->setBounds(0, 0, 420, 1400);
    panel->refresh(bridge->status());
    auto* search = findControl<juce::TextEditor>(*panel, "Search presets");
    auto* factory = findControl<juce::ListBox>(*panel, "Factory presets");
    auto* user = findControl<juce::ListBox>(*panel, "User presets");
    auto* load = findControl<juce::Button>(*panel, "Load into Slot A");
    auto* exportButton = findControl<juce::Button>(*panel, "Export...");
    auto* remove = findControl<juce::Button>(*panel, "Delete");
    auto* catalogStatus = findControl<ui::pc::NoteLabel>(*panel, "Preset catalog status");
    check(search && factory && user && load && exportButton && remove && catalogStatus,
          "production preset controls exist");
    if (!(search && factory && user && load && exportButton && remove && catalogStatus)) return failures;

    const auto filter = [&](const juce::String& query) {
        search->setText(query, false);
        search->onTextChange();
    };
    const auto noSelection = [&] {
        return factory->getSelectedRow() == -1 && user->getSelectedRow() == -1 &&
               !load->isEnabled() && !exportButton->isEnabled() && !remove->isEnabled();
    };
    const auto loadSelected = [&](const char* id, const char* message) {
        const int before = bridge->loaded.size();
        load->onClick();
        check(bridge->loaded.size() == before + 1 && bridge->loaded.getLast() == id, message);
    };

    check(noSelection(), "initial catalog has no implicit action target");
    check(catalogStatus->isVisible() && catalogStatus->text() == bridge->catalogMessage,
          "an incomplete initial catalog exposes the bridge diagnostic");
    user->selectRow(0);
    check(load->isEnabled() && exportButton->isEnabled() && remove->isEnabled(),
          "selecting a user preset enables its actions");
    loadSelected("user:amber", "initial selection loads the visible user preset");

    // The old row index remains valid, but now denotes a different identity.
    filter("Blue");
    check(user->getModel()->getNumRows() == 1 && noSelection(),
          "filtering out the selected ID clears both lists and all selection actions");
    const int beforeFilteredClick = bridge->loaded.size();
    load->onClick();
    check(bridge->loaded.size() == beforeFilteredClick,
          "a cleared selection cannot dispatch the previous hidden preset");
    user->selectRow(0);
    loadSelected("user:blue", "explicit selection after filtering loads the new visible ID");
    check(catalogStatus->isVisible() && catalogStatus->text() == bridge->catalogMessage,
          "selection and load do not clear a catalog diagnostic");

    bridge->catalogMessage.clear();
    filter("");
    check(!catalogStatus->isVisible() && catalogStatus->text().isEmpty(),
          "a complete catalog scan clears the previous diagnostic");
    check(user->getSelectedRow() == 1 && factory->getSelectedRow() == -1,
          "restoring the full catalog follows the selected ID to its new row");
    loadSelected("user:blue", "restoring the catalog preserves the selected action target");

    std::swap(bridge->catalog[0], bridge->catalog[1]);
    filter("");
    check(user->getSelectedRow() == 0,
          "catalog reordering preserves identity rather than the old row index");
    loadSelected("user:blue", "reordered catalog actions still target the selected identity");

    bridge->catalog.erase(std::remove_if(bridge->catalog.begin(), bridge->catalog.end(),
        [](const auto& entry) { return entry.id == "user:blue"; }), bridge->catalog.end());
    filter("");
    check(user->getModel()->getNumRows() == 1 && noSelection(),
          "removing the selected entry clears an otherwise still-valid row index");
    const int beforeRemovedClick = bridge->loaded.size();
    load->onClick();
    check(bridge->loaded.size() == beforeRemovedClick,
          "removed entries cannot remain cached action targets");

    factory->selectRow(0);
    check(load->isEnabled() && exportButton->isEnabled() && !remove->isEnabled(),
          "factory selection keeps delete unavailable");
    loadSelected("factory:clean", "factory selection uses its own ID");

    // Exercise the model callback directly: it must use its explicit row even
    // if a different pane still owns the previous selection.
    const int beforeUserDoubleClick = bridge->loaded.size();
    user->getModel()->listBoxItemDoubleClicked(0, doubleClick(*user));
    check(bridge->loaded.size() == beforeUserDoubleClick + 1 &&
              bridge->loaded.getLast() == "user:amber" && user->getSelectedRow() == 0 &&
              factory->getSelectedRow() == -1 && remove->isEnabled(),
          "user double-click loads and selects its explicit row instead of the old factory ID");
    const int beforeFactoryDoubleClick = bridge->loaded.size();
    factory->getModel()->listBoxItemDoubleClicked(1, doubleClick(*factory));
    check(bridge->loaded.size() == beforeFactoryDoubleClick + 1 &&
              bridge->loaded.getLast() == "factory:warm" && factory->getSelectedRow() == 1 &&
              user->getSelectedRow() == -1 && !remove->isEnabled(),
          "factory double-click loads and selects its explicit row instead of the old user ID");

    filter("Warm");
    check(factory->getSelectedRow() == 0 && user->getSelectedRow() == -1,
          "factory selection also survives filtering by ID");
    loadSelected("factory:warm", "filtered factory action targets the retained identity");
    bridge->catalogMessage = "The user preset directory could not be read.";
    filter("No matching preset");
    check(noSelection(), "an empty catalog clears every selection action");
    check(catalogStatus->isVisible() && catalogStatus->text() == bridge->catalogMessage,
          "an empty catalog still exposes a directory failure");
    return failures;
}

}  // namespace cd::plugin::test::preset_selection_regressions
