// Production panel interactions with an explicitly synthetic tracking bridge.
// Real asynchronous compilation and audio isolation are tested at the bridge boundary.
#pragma once
#include "FakeBridge.h"
#include "ui/panels/PanelCommon.h"
#include <cstdio>
#include <memory>

namespace cd::plugin::test::blueprint_panel_regressions {

class TrackingBridge final : public FakeBridge {
public:
    bool applySupported = false, reportReady = false;
    int cancellations = 0, requests = 0, polls = 0, applications = 0;
    juce::String requestedText;
    BlueprintReport nextReport;

    void requestBlueprintValidation(const juce::String& text) override {
        ++requests;
        requestedText = text;
        reportReady = false;
    }
    void cancelBlueprintValidation() override { ++cancellations; reportReady = false; }
    bool blueprintReport(BlueprintReport& report) override {
        ++polls;
        if (!reportReady) return false;
        reportReady = false;
        report = nextReport;
        return true;
    }
    bool supportsBlueprintApply() const override { return applySupported; }
    bool applyBlueprint(int, const juce::String&, juce::String&) override {
        ++applications;
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

inline int run() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::printf("FAIL blueprint panel: %s\n", message); }
    };
    auto bridge = std::make_unique<TrackingBridge>();
    auto panel = ui::createPanel(ui::PanelId::Blueprint, *bridge);
    panel->setBounds(0, 0, 420, 1800);
    auto* editor = findControl<juce::TextEditor>(*panel, "Blueprint JSON");
    auto* validate = findControl<juce::Button>(*panel, "Validate");
    auto* reload = findControl<juce::Button>(*panel, "Reload");
    auto* example = findControl<juce::Button>(*panel, "Example RC");
    auto* apply = findControl<juce::Button>(*panel, "Apply...");
    auto* state = findControl<ui::pc::NoteLabel>(*panel, "Blueprint state");
    auto* availability = findControl<ui::pc::NoteLabel>(*panel, "Blueprint apply availability");
    auto* counts = findControl<ui::pc::NoteLabel>(*panel, "Blueprint counts");
    auto* estimates = findControl<ui::pc::NoteLabel>(*panel, "Blueprint estimates");
    auto* configuration = findControl<ui::pc::NoteLabel>(*panel, "Blueprint validation configuration");
    auto* models = findControl<ui::pc::NoteLabel>(*panel, "Blueprint model provenance");
    const bool controls = editor && validate && reload && example && apply && state && availability && counts && estimates && configuration && models;
    check(controls, "production blueprint controls exist");
    if (!controls) return failures;

    check(bridge->cancellations == 1 && !apply->isEnabled() && availability->text().contains("unavailable"),
          "initial load cancels old work and explains unavailable application");
    const auto originalText = editor->getText();
    const int beforeExample = bridge->cancellations;
    example->onClick();
    const auto exampleJson = juce::JSON::parse(editor->getText());
    auto* exampleObject = exampleJson.getDynamicObject();
    check(exampleObject && exampleObject->getProperty("id").toString() == "example.rc" &&
              exampleObject->getProperty("modelRegistryVersion").toString() == "fixture-v1" &&
              exampleObject->getProperty("components").size() == 2,
          "Example RC inserts the documented two-component synthetic JSON");
    check(bridge->cancellations == beforeExample + 1 && state->text().contains("State: Draft") &&
              state->text().contains("Synthetic RC") && bridge->requests == 0 && bridge->applications == 0,
          "example loading only changes the draft and cancels obsolete validation");

    validate->onClick();
    check(bridge->requests == 1 && bridge->requestedText == editor->getText() &&
              state->text().contains("State: Validating") && !apply->isEnabled(),
          "validation submits the current draft without applying it");
    const int beforeEdit = bridge->cancellations;
    editor->setText(editor->getText() + "\n", false);
    editor->onTextChange();
    const int pollsAfterEdit = bridge->polls;
    panel->refresh(bridge->status());
    check(bridge->cancellations == beforeEdit + 1 && bridge->polls == pollsAfterEdit &&
              state->text().contains("State: Draft") && counts->text().contains("No validation report") && !apply->isEnabled(),
          "editing cancels the old request and prevents its report from becoming actionable");

    bridge->nextReport.ok = true;
    bridge->nextReport.components = 2;
    bridge->nextReport.nodes = 3;
    bridge->nextReport.unknowns = 3;
    bridge->nextReport.estimatedFlopsPerSample = 108;
    bridge->nextReport.estimatedBytes = 4096;
    bridge->nextReport.estimatedCostNsPerSample = 999;  // Legacy field must not be presented as measured CPU time.
    bridge->nextReport.sampleRate = 48000;
    bridge->nextReport.oversampling = 4;
    bridge->nextReport.syntheticModels = true;
    bridge->nextReport.previewConfiguration = true;
    validate->onClick();
    bridge->reportReady = true;
    panel->refresh(bridge->status());
    check(state->text().contains("State: Ready") && counts->text().contains("Components 2") && !apply->isEnabled(),
          "a valid preview remains unavailable for live application");
    check(estimates->text().contains("108 FLOPs per electrical sample") && estimates->text().contains("4096 bytes") &&
              !estimates->text().contains("999") && !estimates->text().contains("ns/sample"),
          "the report shows arithmetic and storage estimates without inventing CPU timing");
    check(configuration->text().contains("Preview configuration") && configuration->text().contains("48000 Hz") &&
              configuration->text().contains("4x") && models->text().contains("synthetic validation fixtures"),
          "the report identifies its Fs/OS, preview context, and synthetic models");
    const int modalCount = juce::Component::getNumCurrentlyModalComponents();
    apply->onClick();
    check(bridge->applications == 0 && juce::Component::getNumCurrentlyModalComponents() == modalCount,
          "an unavailable Apply callback opens no confirmation and dispatches nothing");

    bridge->applySupported = true;
    bridge->nextReport.previewConfiguration = false;
    validate->onClick();
    bridge->reportReady = true;
    panel->refresh(bridge->status());
    check(apply->isEnabled() && configuration->text().contains("Audio configuration") &&
              !configuration->text().contains("Preview configuration"),
          "a bridge that supports applying retains the existing Ready behavior");
    bridge->applySupported = false;
    apply->onClick();
    check(bridge->applications == 0 && juce::Component::getNumCurrentlyModalComponents() == modalCount,
          "Apply rechecks capability even if a previously enabled button callback fires");

    const int beforeReload = bridge->cancellations;
    reload->onClick();
    check(bridge->cancellations == beforeReload + 1 && editor->getText() == originalText &&
              state->text().contains("State: Idle") && !apply->isEnabled() && !estimates->isVisible(),
          "reload cancels validation and clears the previous report");
    validate->onClick();
    const int beforeSlotChange = bridge->cancellations;
    bridge->setEditSlot(1);
    panel->refresh(bridge->status());
    check(bridge->cancellations == beforeSlotChange + 1 && state->text().contains("State: Idle"),
          "changing slots during validation cancels and reloads the draft");
    validate->onClick();
    const int beforeDestruction = bridge->cancellations;
    panel.reset();
    check(bridge->cancellations == beforeDestruction + 1 && bridge->applications == 0,
          "closing the panel cancels outstanding work and no validation action applies audio");
    return failures;
}

}  // namespace cd::plugin::test::blueprint_panel_regressions
