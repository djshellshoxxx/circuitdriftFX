// CircuitDrift plugin UI: component inspector (CD-F17). Identity, role, material, model,
// provenance, nominal/base/effective values with units, live telemetry, condition/fault state,
// numeric entry and material selector, and bundled education text. Message thread only.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "ui/Theme.h"
#include "ui/workbench/PcbCanvas.h"

namespace cd::plugin::ui {

class InspectorPanel final : public juce::Component {
public:
    InspectorPanel(EditorBridge& bridge, PcbCanvas& canvas);
    void setTheme(const Theme& t);
    void setEducational(bool on) { educational_ = on; relayout(); }
    // Called after selection changes and once per UI frame.
    void refresh();
    // Numeric entry for the selected component's primary editable value (also what Enter in the
    // field does). Returns false and sets lastError() without touching state when invalid.
    bool applyNumeric(const juce::String& text);
    juce::String lastError() const { return error_; }
    // Everything drawn in the last layout pass (tests assert on units/labels).
    juce::String drawnText() const { return drawn_.joinIntoString("\n"); }
    int contentHeight() const noexcept { return contentH_; }
    void paint(juce::Graphics& g) override;
    void resized() override { relayout(); }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;

private:
    void relayout();
    int layoutPass(juce::Graphics* g);   // returns content height; positions child controls
    void syncField(bool force);
    juce::String fieldUnitLabel() const;

    EditorBridge& bridge_;
    PcbCanvas& canvas_;
    ParamOps ops_;
    const Theme* theme_ = &darkTheme();
    bool educational_ = false;
    int contentH_ = 0, scrollY_ = 0;
    juce::StringArray drawn_;
    juce::TextEditor field_;
    juce::TextButton resetButton_{"Reset"}, material_[3];
    juce::String error_;
    int fieldFor_ = -2;
};

}  // namespace cd::plugin::ui
