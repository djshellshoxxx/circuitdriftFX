// Live EditorBridge regression: real APVTS edits, bounded history and custom editor.
// Synthetic model/host validation is not implied by this message-thread test.
#include "PluginProcessor.h"
#include "ui/PluginEditor.h"
#include "ui/ProcessorEditorBridge.h"
#include "ui/panels/PanelCommon.h"
#include "ui/workbench/ParamOps.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <chrono>
#include <thread>

using namespace cd::plugin;
namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
bool closeTo(float a, float b) { return std::abs(a - b) < 0.00002f; }
void write(juce::RangedAudioParameter& p, float normalized) {
    p.beginChangeGesture();
    p.setValueNotifyingHost(normalized);
    p.endChangeGesture();
}
bool waitForReport(ProcessorEditorBridge& bridge, BlueprintReport& report) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (bridge.blueprintReport(report)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    CircuitDriftProcessor processor(Edition::Full, "CircuitDrift");
    ProcessorEditorBridge bridge(processor);
    auto telemetry = std::make_unique<cd::engine::TelemetryFrame>();
    check(!bridge.readTelemetry(*telemetry),
          "unprepared processor cannot publish synthetic telemetry");

    auto* a = processor.parameters().getParameter("slot.a.stage.drive");
    auto* b = processor.parameters().getParameter("slot.b.stage.drive");
    auto* crossfade = processor.parameters().getParameter("ab.crossfade");
    check(a != nullptr && b != nullptr && crossfade != nullptr, "A/B live parameters exist");
    if (a && b && crossfade) {
        const float initialA = a->getValue(), initialB = b->getValue();
        const float changed = initialA < 0.4f ? 0.8f : 0.1f;
        bridge.beginGesture("Drive A");
        bridge.touchParameter(a->getParameterID());
        write(*a, changed);
        bridge.endGesture();
        check(bridge.history().canUndo && bridge.history().position == 1, "gesture recorded once");
        bridge.undo();
        check(closeTo(a->getValue(), initialA) && bridge.history().canRedo, "undo restores old value");
        bridge.redo();
        check(closeTo(a->getValue(), changed), "redo restores new value");
        bridge.copySlot(0, 1);
        check(closeTo(b->getValue(), changed), "A-to-B copy changes real host parameter");
        bridge.undo();
        check(closeTo(b->getValue(), initialB), "copy is reversible as one transaction");
        bridge.redo();
        check(closeTo(b->getValue(), changed), "copy redo");
        write(*b, changed < 0.4f ? 0.9f : 0.1f);
        const float beforeSwapA = a->getValue(), beforeSwapB = b->getValue();
        const float fade = crossfade->getValue();
        bridge.swapSlots();
        check(closeTo(a->getValue(), beforeSwapB) && closeTo(b->getValue(), beforeSwapA),
              "swap exchanges both live slot parameters");
        check(closeTo(crossfade->getValue(), 1.0f - fade), "swap mirrors crossfade");
        bridge.undo();
        check(closeTo(a->getValue(), beforeSwapA) && closeTo(b->getValue(), beforeSwapB) &&
              closeTo(crossfade->getValue(), fade), "swap undo restores slots and crossfade");

        // An unrelated host parameter may move while a UI gesture is still open.
        // Only the user's declared parameter belongs to that gesture's undo step.
        {
            const float initial = a->getValue();
            ui::pc::ParamBinding binding(bridge, "Scoped drive edit");
            binding.bind(a->getParameterID(), [](float) {});
            binding.beginPart();
            binding.setPart(a->convertFrom0to1(0.4f));
            write(*b, 0.65f);
            binding.endPart();
            bridge.undo();
            check(closeTo(a->getValue(), initial) && closeTo(b->getValue(), 0.65f),
                  "panel undo leaves unrelated host automation unchanged");
            bridge.redo();
            check(closeTo(a->getValue(), 0.4f) && closeTo(b->getValue(), 0.65f),
                  "panel redo reapplies only the declared parameter");
        }
        {
            bridge.setEditSlot(0);
            const float initial = a->getValue();
            ui::ParamOps ops(bridge);
            check(ops.begin("Scoped workbench drive", {"stage.drive", "stage.drive"}),
                  "real workbench gesture opens");
            ops.setNormalized("stage.drive", 0.25f);
            write(*b, 0.75f);
            ops.end();
            bridge.undo();
            check(closeTo(a->getValue(), initial) && closeTo(b->getValue(), 0.75f),
                  "workbench undo retains first captured value and excludes unrelated automation");
        }
        {
            const float initial = a->getValue();
            bridge.beginGesture("Repeated declaration");
            bridge.touchParameter(a->getParameterID());
            write(*a, 0.2f);
            bridge.touchParameter(a->getParameterID());
            write(*a, 0.3f);
            bridge.endGesture();
            bridge.undo();
            check(closeTo(a->getValue(), initial), "repeated touch keeps the first pre-edit value");
        }

        // Host automation after a local transaction must never be overwritten by undo.
        bridge.beginGesture("Host conflict");
        bridge.touchParameter(a->getParameterID());
        write(*a, 0.45f);
        bridge.endGesture();
        write(*a, 0.65f);
        bridge.undo();
        check(closeTo(a->getValue(), 0.65f) && !bridge.history().canUndo,
              "stale local undo rejected rather than overwriting host changes");

        bridge.beginGesture("Session load invalidation");
        bridge.touchParameter(a->getParameterID());
        write(*a, 0.3f);
        bridge.endGesture();
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(!bridge.history().canUndo && !bridge.history().canRedo,
              "replacing host session invalidates local undo stack");
    }

    // Retention: a new gesture after twenty steps evicts the oldest without growing unbounded.
    if (a) {
        for (int i = 0; i < 23; ++i) {
            bridge.beginGesture("Bounded edit " + juce::String(i));
            bridge.touchParameter(a->getParameterID());
            write(*a, 0.10f + static_cast<float>(i) * 0.02f);
            bridge.endGesture();
        }
        check(bridge.history().entries.size() == 20 && bridge.history().position == 20,
              "history retention capped at twenty completed transactions");
    }

    check(!bridge.supportsEngineReset(), "unprepared live processor disables engine reset");
    {
        auto quality = ui::pc::makeQualityPanel(bridge);
        int disabledResets = 0;
        const auto inspect = [&](auto&& self, juce::Component& component) -> void {
            if (auto* button = dynamic_cast<juce::TextButton*>(&component))
                if ((button->getButtonText() == "Reset Warm" || button->getButtonText() == "Reset Cold") &&
                    !button->isEnabled()) ++disabledResets;
            for (auto* child : component.getChildren()) self(self, *child);
        };
        inspect(inspect, *quality);
        check(disabledResets == 2, "quality panel exposes disabled reset actions before preparation");
    }
    check(!bridge.supportsRestorationCommit(),
          "live UI must disable unavailable seed-restoration action");
    juce::String error;
    check(!bridge.loadPreset("factory:clean-console", 0, error) && error.isNotEmpty(),
          "scene apply remains disabled until slot seed/blueprint transactions exist");
    // Exercise the real local library with an injected temporary directory, not
    // the developer's own presets. Factory lookup, import/export and deletion are
    // independent of the missing sound-apply service.
    const auto tempRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("cdfx-preset-service", "", true);
    check(tempRoot.createDirectory(), "create isolated preset test directory");
    {
        ProcessorEditorBridge catalog(processor, std::filesystem::u8path(tempRoot.getFullPathName().toStdString()));
        const auto factories = catalog.listPresets("factory:clean-console");
        check(factories.size() == 1 && factories[0].factory &&
              factories[0].id == "factory:clean-console", "real factory catalog and search");
        const auto sourceDir = tempRoot.getChildFile("source");
        check(sourceDir.createDirectory(), "create separate export source directory");
        const auto exported = sourceDir.getChildFile("preview.circuitdrift.json");
        error.clear();
        check(catalog.exportPreset("factory:clean-console", exported, error) && exported.existsAsFile(),
              "export real factory scene to canonical JSON");
        error.clear();
        check(catalog.importPreset(exported, error), "import validated local scene without overwrite");
        const auto users = catalog.listPresets("user:preview");
        check(users.size() == 1 && !users[0].factory && users[0].category == "User",
              "imported scene is discoverable as user preset");
        const auto external = tempRoot.getChildFile("External.circuitdrift.json");
        check(exported.copyFileTo(external), "add externally saved preset after catalog snapshot");
        check(catalog.listPresets("user:External").empty(), "typing filters the existing snapshot without disk reads");
        catalog.refreshPresets();
        check(catalog.listPresets("user:External").size() == 1, "explicit refresh discovers external changes");
        error.clear();
        check(!catalog.importPreset(exported, error) && error.isNotEmpty(),
              "duplicate import never overwrites existing preset");
        error.clear();
        check(!catalog.deletePreset("factory:clean-console", error) && error.isNotEmpty(),
              "factory scenes remain read-only");
        error.clear();
        check(catalog.deletePreset("user:preview", error) && catalog.listPresets("user:preview").empty(),
              "delete only the selected user preset");
        juce::Image image(juce::Image::ARGB, 4, 4, true);
        const auto screenshot = tempRoot.getChildFile("capture.png");
        error.clear();
        check(catalog.savePng(image, screenshot, error) && screenshot.existsAsFile(),
              "PNG export writes through atomic local-file replacement");
        juce::FileInputStream pngStream(screenshot);
        juce::PNGImageFormat decoder;
        check(pngStream.openedOk() && decoder.decodeImage(pngStream).isValid(),
              "atomic PNG contains decodable image bytes");
        error.clear();
        check(!catalog.savePng(juce::Image{}, screenshot, error) && screenshot.existsAsFile(),
              "invalid capture does not destroy an existing file");
    }
    check(tempRoot.deleteRecursively(), "remove isolated preset test directory");
    error.clear();
    check(!bridge.applyBlueprint(0, "{}", error) && error.isNotEmpty(),
          "unwired runtime blueprint installation fails explicitly");
    bridge.requestBlueprintValidation("{}");
    BlueprintReport report;
    check(waitForReport(bridge, report) && !report.ok && !report.diagnostics.isEmpty(),
          "background validator returns real malformed-document diagnostics");
    check(!bridge.supportsBlueprintApply(), "validation preview does not claim live graph installation");
    const juce::String fixture = R"({"schemaVersion":1,"id":"live.rc","name":"Live RC fixture","modelRegistryVersion":"fixture-v1",
      "nodes":["0","in","out"],"components":[
        {"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":10000}},
        {"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8}}],
      "ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})";
    processor.prepareToPlay(8000, 32);
    juce::AudioBuffer<float> silent(2, 32);
    silent.clear();
    juce::MidiBuffer noMidi;
    processor.processBlock(silent, noMidi);
    juce::MemoryBlock beforeValidation, afterValidation;
    processor.getStateInformation(beforeValidation);
    bridge.requestBlueprintValidation(fixture);
    check(waitForReport(bridge, report) && report.ok, "live validator compiles a supported synthetic circuit");
    check(report.components == 2 && report.nodes == 3 && report.unknowns == 3 &&
          report.estimatedFlopsPerSample > 0 && report.estimatedCostNsPerSample == 0 &&
          report.configuration.contains("synthetic fixture-v1"), "report exposes real cost units and model provenance");
    check(!bridge.blueprintReport(report), "live report is consumed once");
    processor.getStateInformation(afterValidation);
    check(beforeValidation == afterValidation, "validation leaves saved sound and host parameters unchanged");
    bridge.requestBlueprintValidation(fixture);
    processor.setStateInformation(beforeValidation.getData(), static_cast<int>(beforeValidation.getSize()));
    check(waitForReport(bridge, report) && !report.ok &&
          report.diagnostics.joinIntoString(" ").contains("changed"), "session replacement invalidates pending validation");
    processor.releaseResources();

    UiSettings prefs;
    prefs.scale = 500.0f;
    prefs.lastWidth = -1;
    bridge.setUiSettings(prefs);
    check(bridge.uiSettings().scale == 2.0f && bridge.uiSettings().lastWidth == 750,
          "UI settings are bounded");
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        check(editor != nullptr && editor->getWidth() >= 750 && editor->getNumChildComponents() > 0,
              "real processor constructs the custom editor, not GenericAudioProcessorEditor");
    }
    CircuitDriftProcessor lite(Edition::Lite, "CircuitDrift LITE");
    ProcessorEditorBridge liteBridge(lite);
    liteBridge.copySlot(0, 1);
    liteBridge.swapSlots();
    check(!liteBridge.history().canUndo, "LITE cannot mutate unavailable A/B slots");

    std::printf("live EditorBridge: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
