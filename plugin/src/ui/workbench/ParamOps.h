// CircuitDrift plugin UI: parameter edits for Workbench gestures (UX-01). One gesture = exactly one
// bridge.beginGesture/endGesture transaction (one undo record) plus the matching host change
// gestures. Escape restores every parameter touched since the transaction began.
#pragma once
#include <map>
#include <vector>
#include "ui/EditorBridge.h"

namespace cd::plugin::ui {

class ParamOps {
public:
    explicit ParamOps(EditorBridge& b) : bridge_(b) {}
    ~ParamOps() { if (active_) end(); }

    juce::RangedAudioParameter* param(const juce::String& baseName) const;   // edit-slot parameter
    bool exists(const juce::String& baseName) const { return param(baseName) != nullptr; }
    double normalized(const juce::String& baseName) const;                   // 0..1 host range
    double real(const juce::String& baseName) const;                         // plain units
    double defaultNormalized(const juce::String& baseName) const;

    bool active() const noexcept { return active_; }
    // Starts the single transaction for the listed edit-slot parameters (their current values are
    // remembered for rollback). Returns false if one is missing or a transaction is already open.
    bool begin(const juce::String& label, const std::vector<juce::String>& baseNames);
    void setNormalized(const juce::String& baseName, double normalized);
    void setReal(const juce::String& baseName, double plain);
    void end();        // commit the last value, close host gestures and the bridge transaction
    void rollback();   // Escape: restore the values captured at begin(), then close
    // Convenience for keyboard/numeric edits: begin + set + end.
    bool setOnce(const juce::String& label, const juce::String& baseName, double normalized);

private:
    EditorBridge& bridge_;
    bool active_ = false;
    std::vector<std::pair<juce::RangedAudioParameter*, float>> touched_;
};

// Which edit-slot parameter a component's direct manipulation / arrow keys / numeric field drive.
// nullptr when the component has no editable parameter.
const char* editParamFor(int componentId) noexcept;

}  // namespace cd::plugin::ui
