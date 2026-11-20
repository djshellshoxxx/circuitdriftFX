// Real asynchronous compilation through the message-thread bridge. All models
// and circuits are synthetic fixtures, never physical-device qualification.
#pragma once
#include "PluginProcessor.h"
#include "ui/BlueprintValidationService.h"
#include "ui/ProcessorEditorBridge.h"
#include "ui/panels/PanelCommon.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <thread>

namespace cd::plugin::test::blueprint_validation_regressions {

// Same synthetic topology as docs/planned-features/examples/rc-blueprint.json.
inline constexpr const char* kRc = R"({"schemaVersion":1,"id":"example.rc","name":"Synthetic RC validation fixture","modelRegistryVersion":"fixture-v1",
"nodes":["0","in","out"],"components":[
{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":10000}},
{"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8}}],
"ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})";

template <class Read>
bool waitForReport(Read&& read, BlueprintReport& report) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do {
        if (read(report)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

inline bool hasDiagnostic(const BlueprintReport& report, const char* text) {
    return report.diagnostics.joinIntoString("\n").contains(text);
}

template <class T>
T* control(juce::Component& root, const juce::String& title) {
    for (auto* child : root.getChildren()) {
        if (child->getTitle() == title)
            if (auto* match = dynamic_cast<T*>(child)) return match;
        if (auto* match = control<T>(*child, title)) return match;
    }
    return nullptr;
}

inline bool hasNote(juce::Component& root, const char* text) {
    if (auto* note = dynamic_cast<ui::pc::NoteLabel*>(&root))
        if (note->text().contains(text)) return true;
    for (auto* child : root.getChildren()) if (hasNote(*child, text)) return true;
    return false;
}

inline int run() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::fprintf(stderr, "FAIL blueprint validation: %s\n", message); }
    };
    auto processor = std::make_unique<CircuitDriftProcessor>(Edition::Full, "CircuitDrift");
    ProcessorEditorBridge bridge(*processor);
    const auto read = [&](BlueprintReport& result) { return bridge.blueprintReport(result); };
    BlueprintReport report;
    report.components = 913;
    report.diagnostics.add("unchanged");
    check(!read(report) && report.components == 913 && hasDiagnostic(report, "unchanged"),
          "no request leaves the caller's report unchanged");
    check(!bridge.supportsBlueprintApply() && bridge.blueprintJson(0).isEmpty(),
          "validation never represents the example as the current audio circuit");

    juce::MemoryBlock before, after;
    processor->getStateInformation(before);
    const auto historyBefore = bridge.history();
    bridge.requestBlueprintValidation(kRc);
    check(waitForReport(read, report) && report.ok && report.components == 2 && report.nodes == 3 &&
              report.nonlinear == 0 && report.unknowns == 3 && report.diagnostics.isEmpty(),
          "valid synthetic RC runs all real compiler checks");
    check(report.sampleRate == 48000 && report.oversampling == 1 && report.previewConfiguration &&
              report.syntheticModels && report.estimatedFlopsPerSample > 0 && report.estimatedBytes > 0 &&
              report.estimatedCostNsPerSample == 0,
          "unprepared validation labels preview settings and truthful arithmetic/memory estimates");
    check(!read(report) && report.ok && report.components == 2,
          "a completed report is delivered once and then left unchanged");
    processor->getStateInformation(after);
    check(before.getSize() == after.getSize() &&
              std::memcmp(before.getData(), after.getData(), before.getSize()) == 0 &&
              bridge.history().position == historyBefore.position && !bridge.history().canUndo,
          "validation changes neither persisted host parameters/preferences nor undo history");
    auto telemetry = std::make_unique<engine::TelemetryFrame>();
    check(!bridge.readTelemetry(*telemetry), "validation never prepares or fabricates audio telemetry");

    bridge.requestBlueprintValidation("{");
    check(waitForReport(read, report) && !report.ok && hasDiagnostic(report, "[Malformed]"),
          "malformed input reports the real compiler diagnostic");
    bridge.requestBlueprintValidation(juce::String(kRc).replace("fixture.capacitor", "fixture.inductor"));
    check(waitForReport(read, report) && !report.ok && hasDiagnostic(report, "[UnsupportedModel]") &&
              hasDiagnostic(report, "C1"), "unsupported model reports its component identity");
    bridge.requestBlueprintValidation(juce::String(kRc).replace("\"n\":\"0\"", "\"n\":\"missing\""));
    check(waitForReport(read, report) && !report.ok && hasDiagnostic(report, "[PinNode]") &&
              hasDiagnostic(report, "C1.n"), "invalid connection reports its component and pin");

    // Admission uses UTF-8 bytes, before the std::string copy.
    const auto oversized = juce::String::repeatedString(juce::String::charToString(0x20ac),
        static_cast<int>(blueprint::kMaxBytes / 3 + 1));
    check(static_cast<std::size_t>(oversized.length()) < blueprint::kMaxBytes &&
              oversized.getNumBytesAsUTF8() > blueprint::kMaxBytes, "UTF-8 fixture crosses the byte bound only");
    bridge.requestBlueprintValidation(oversized);
    check(read(report) && !report.ok && hasDiagnostic(report, "1 MiB"),
          "oversized UTF-8 receives an immediate explicit rejection");

    // Replacing pending/active work also clears already-completed reports.
    for (int i = 0; i < 40; ++i) bridge.requestBlueprintValidation(i % 2 ? "{" : kRc);
    bridge.requestBlueprintValidation(kRc);
    check(waitForReport(read, report) && report.ok && report.components == 2 && !read(report),
          "only the final request can publish after rapid replacement");
    bridge.requestBlueprintValidation(kRc);
    bridge.cancelBlueprintValidation();
    bridge.requestBlueprintValidation("{");
    check(waitForReport(read, report) && !report.ok && hasDiagnostic(report, "[Malformed]") && !read(report),
          "cancelled work cannot overwrite the next request's failure");
    bridge.requestBlueprintValidation(oversized);
    bridge.cancelBlueprintValidation();
    check(!read(report), "cancellation discards a ready immediate failure too");

    bridge.requestBlueprintValidation(kRc);
    processor->setStateInformation(before.getData(), static_cast<int>(before.getSize()));
    check(waitForReport(read, report) && !report.ok && hasDiagnostic(report, "host session changed"),
          "host session replacement invalidates a pending validation");
    processor->prepareToPlay(16000, 64);
    juce::AudioBuffer<float> samples(2, 64);
    juce::MidiBuffer midi;
    samples.clear();
    processor->processBlock(samples, midi);
    const auto status = bridge.status();
    bridge.requestBlueprintValidation(kRc);
    check(waitForReport(read, report) && report.ok && !report.previewConfiguration &&
              report.sampleRate == status.sampleRate && report.sampleRate == 16000 &&
              report.oversampling == status.activeOversampling,
          "prepared validation uses the active audio configuration snapshot");
    bridge.requestBlueprintValidation(kRc);
    processor->prepareToPlay(32000, 64);
    check(waitForReport(read, report) && !report.ok && hasDiagnostic(report, "audio configuration"),
          "a changed audio configuration invalidates a pending report");
    processor->releaseResources();

    // Production panel, bridge, worker, compiler and diagnostics together.
    {
        auto panel = ui::createPanel(ui::PanelId::Blueprint, bridge);
        panel->setBounds(0, 0, 420, 1400);
        auto* example = control<juce::Button>(*panel, "Example RC");
        auto* validate = control<juce::Button>(*panel, "Validate");
        auto* apply = control<juce::Button>(*panel, "Apply...");
        auto* editor = control<juce::TextEditor>(*panel, "Blueprint JSON");
        check(example && validate && apply && editor, "production blueprint controls are available");
        if (example && validate && apply && editor) {
            example->onClick();
            check(editor->getText().contains("example.rc") && !apply->isEnabled(),
                  "example creates an editable synthetic draft only");
            validate->onClick();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            do {
                panel->refresh(bridge.status());
                if (hasNote(*panel, "State: Ready") || hasNote(*panel, "State: Invalid")) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            check(hasNote(*panel, "State: Ready") && hasNote(*panel, "Components 2") &&
                      hasNote(*panel, "FLOPs") && !apply->isEnabled(),
                  "real successful validation reaches Ready while Apply remains unavailable");
            validate->onClick();
            editor->setText("{", false);
            editor->onTextChange();
            check(hasNote(*panel, "State: Draft") && !apply->isEnabled() && !read(report),
                  "editing cancels the in-flight result and clears readiness");
            validate->onClick();
        }
    }
    check(!read(report), "destroying the blueprint panel cancels its pending validation");
    auto lite = std::make_unique<CircuitDriftProcessor>(Edition::Lite, "CircuitDrift LITE");
    ProcessorEditorBridge liteBridge(*lite);
    liteBridge.requestBlueprintValidation(kRc);
    check(liteBridge.blueprintReport(report) && !report.ok && hasDiagnostic(report, "Full edition"),
          "LITE rejects blueprint compilation explicitly");

    // Shutdown belongs to the service. No editor, processor or UI callback is
    // retained by the worker. A generous deadline catches a missing cancellation.
    std::promise<void> destroyed;
    auto done = destroyed.get_future();
    std::thread owner([&] {
        {
            BlueprintValidationService service;
            service.request(kRc, {384000, 16, 1.0});
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        destroyed.set_value();
    });
    if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        std::fprintf(stderr, "FAIL blueprint validation: worker shutdown exceeded 10 seconds\n");
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
    owner.join();
    return failures;
}

}  // namespace cd::plugin::test::blueprint_validation_regressions
