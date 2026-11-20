// CircuitDrift plugin: top-level editor (contract owned by the coordinator; implementation by the
// shell owner in plugin/src/ui/PluginEditor.cpp and plugin/src/ui/shell/**).
// Threading: message thread only. Talks to the processor exclusively through EditorBridge.
#pragma once
#include <memory>
#include "ui/EditorBridge.h"

namespace cd::plugin::ui {

class CircuitDriftEditor : public juce::AudioProcessorEditor {
public:
    CircuitDriftEditor(juce::AudioProcessor& processor, EditorBridge& bridge);
    ~CircuitDriftEditor() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cd::plugin::ui
