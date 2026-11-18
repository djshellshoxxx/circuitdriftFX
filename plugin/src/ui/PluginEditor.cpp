// CircuitDrift plugin: top-level editor. A thin AudioProcessorEditor around shell::EditorCore, which
// owns layout, header/status, macros, utilities, Easy view, drawer and the 60/30 Hz refresh.
// Logical base size 1000x720, minimum 750x540, user scale 75..200 % (independent of OS DPI).
#include "ui/PluginEditor.h"

#include "ui/shell/EditorCore.h"

namespace cd::plugin::ui {

struct CircuitDriftEditor::Impl {
    Impl(CircuitDriftEditor& editor, EditorBridge& bridge)
        : core(editor, bridge, constrainer), tooltips(&editor, 450) {}
    juce::ComponentBoundsConstrainer constrainer;
    shell::EditorCore core;
    // Hosts standard juce::SettableTooltipClient tooltips used by the workbench and panels
    // (450 ms delay, CD-F08); the shell's own educational help overlay is separate.
    juce::TooltipWindow tooltips;
};

CircuitDriftEditor::CircuitDriftEditor(juce::AudioProcessor& processor, EditorBridge& bridge)
    : juce::AudioProcessorEditor(processor) {
    // Initial size from the saved per-instance settings (logical size x user scale).
    const auto s = bridge.uiSettings();
    const float scale = juce::jlimit(shell::kMinScale, shell::kMaxScale, s.scale);
    const int w = juce::roundToInt(std::max(s.lastWidth, shell::kMinWidth) * scale);
    const int h = juce::roundToInt(std::max(s.lastHeight, shell::kMinHeight) * scale);
    setSize(w, h);
    impl_ = std::make_unique<Impl>(*this, bridge);
    setConstrainer(&impl_->constrainer);
    setResizable(true, true);
    impl_->core.setActive(isShowing());
}

CircuitDriftEditor::~CircuitDriftEditor() {
    impl_->core.setActive(false);
    setConstrainer(nullptr);
}

void CircuitDriftEditor::paint(juce::Graphics& g) {
    g.fillAll(impl_ ? themeFor(impl_->core.context().ui.darkTheme).background : juce::Colours::black);
}

void CircuitDriftEditor::resized() {
    if (impl_) impl_->core.hostResized();
}

}  // namespace cd::plugin::ui
