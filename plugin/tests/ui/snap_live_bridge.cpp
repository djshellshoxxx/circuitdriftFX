// Live EditorBridge regression: real APVTS edits, bounded history and custom editor.
// Synthetic model/host validation is not implied by this message-thread test.
#include "PluginProcessor.h"
#include "ui/PluginEditor.h"
#include "ui/ProcessorEditorBridge.h"
#include <cmath>
#include <cstdio>
#include <memory>

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
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    CircuitDriftProcessor processor(Edition::Full, "CircuitDrift");
    ProcessorEditorBridge bridge(processor);
    check(!bridge.readTelemetry(*std::make_unique<cd::engine::TelemetryFrame>()),
          "unprepared processor cannot publish synthetic telemetry");

    auto* a = processor.parameters().getParameter("slot.a.stage.drive");
    auto* b = processor.parameters().getParameter("slot.b.stage.drive");
    auto* crossfade = processor.parameters().getParameter("ab.crossfade");
    check(a != nullptr && b != nullptr && crossfade != nullptr, "A/B live parameters exist");
    if (a && b && crossfade) {
        const float initialA = a->getValue(), initialB = b->getValue();
        const float changed = initialA < 0.4f ? 0.8f : 0.1f;
        bridge.beginGesture("Drive A");
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

        // Host automation after a local transaction must never be overwritten by undo.
        bridge.beginGesture("Host conflict");
        write(*a, 0.45f);
        bridge.endGesture();
        write(*a, 0.65f);
        bridge.undo();
        check(closeTo(a->getValue(), 0.65f) && !bridge.history().canUndo,
              "stale local undo rejected rather than overwriting host changes");

        bridge.beginGesture("Session load invalidation");
        write(*a, 0.3f);
        bridge.endGesture();
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(!bridge.history().canUndo && !bridge.history().canRedo,
              "replacing host session invalidates local undo stack");
    }

    juce::String error;
    check(!bridge.loadPreset("factory:clean-console", 0, error) && error.isNotEmpty(),
          "unwired preset service fails explicitly");
    error.clear();
    check(!bridge.applyBlueprint(0, "{}", error) && error.isNotEmpty(),
          "unwired runtime blueprint installation fails explicitly");
    bridge.requestBlueprintValidation("{}");
    BlueprintReport report;
    check(bridge.blueprintReport(report) && !report.ok && !report.diagnostics.isEmpty(),
          "unwired validation reports a failure, not a fabricated success");

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
