// Contract smoke test: the editor, workbench and every panel construct against FakeBridge and
// render headlessly in both themes. Area owners add their own snap_<area>.cpp with real checks.
#include "FakeBridge.h"
#include "ui/PluginEditor.h"
#include "ui/panels/Panels.h"
#include "ui/workbench/WorkbenchView.h"

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    int failures = 0;
    cd::plugin::test::FakeBridge bridge;
    {
        cd::plugin::ui::CircuitDriftEditor editor(bridge.processor(), bridge);
        auto img = cd::plugin::test::renderComponent(editor, 1000, 720, 1.0f);
        if (!img.isValid() || !cd::plugin::test::writePng(img, outDir.getChildFile("contract_editor.png"))) ++failures;
    }
    {
        cd::plugin::ui::WorkbenchView wb(bridge);
        engine::TelemetryFrame f; bridge.readTelemetry(f); wb.updateFrame(f, true);
        if (!cd::plugin::test::renderComponent(wb, 800, 500, 1.0f).isValid()) ++failures;
    }
    for (int i = 0; i < static_cast<int>(cd::plugin::ui::PanelId::Count); ++i) {
        auto p = cd::plugin::ui::createPanel(static_cast<cd::plugin::ui::PanelId>(i), bridge);
        if (!p || juce::String(cd::plugin::ui::panelTitle(static_cast<cd::plugin::ui::PanelId>(i))).isEmpty()) { ++failures; continue; }
        p->refresh(bridge.status());
        if (!cd::plugin::test::renderComponent(*p, 400, 500, 1.0f).isValid()) ++failures;
    }
    std::printf(failures ? "UI contract snapshot FAILED (%d)\n" : "UI contract snapshot passed\n", failures);
    return failures ? 1 : 0;
}
