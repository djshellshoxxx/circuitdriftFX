// Temporary minimal editor so the tree links before the shell implementation lands.
// The shell owner replaces this file.
#include "ui/PluginEditor.h"
#include "ui/Theme.h"

namespace cd::plugin::ui {

struct CircuitDriftEditor::Impl {
    explicit Impl(EditorBridge& b) : bridge(b) {}
    EditorBridge& bridge;
};

CircuitDriftEditor::CircuitDriftEditor(juce::AudioProcessor& processor, EditorBridge& bridge)
    : juce::AudioProcessorEditor(processor), impl_(std::make_unique<Impl>(bridge)) {
    setSize(1000, 720);
}
CircuitDriftEditor::~CircuitDriftEditor() = default;
void CircuitDriftEditor::paint(juce::Graphics& g) {
    const auto& t = themeFor(true);
    g.fillAll(t.background);
    g.setColour(t.text);
    g.drawText("CircuitDrift", getLocalBounds(), juce::Justification::centred);
}
void CircuitDriftEditor::resized() {}

}  // namespace cd::plugin::ui
