// CircuitDrift plugin: Workbench view (CD-F17 inspector, UX-01 gestures, UX-04/05/06 displays).
// Contract (coordinator-owned): class name, constructor and the public methods below are stable;
// the workbench owner implements them in plugin/src/ui/workbench/*.cpp.
// Threading: message thread only. Reads telemetry through EditorBridge; never touches audio state.
#pragma once
#include <memory>
#include "ui/EditorBridge.h"

namespace cd::plugin::ui {

class WorkbenchView : public juce::Component {
public:
    explicit WorkbenchView(EditorBridge& bridge);
    ~WorkbenchView() override;
    // Called by the editor at UI frame rate (30/60 Hz) with the latest telemetry, so all views
    // share one snapshot per frame (sample-timestamp aligned) instead of polling independently.
    void updateFrame(const engine::TelemetryFrame& frame, bool frameValid);
    // Theme/scale/settings changes (editor applies the transform; this re-styles content).
    void settingsChanged(const UiSettings& settings);
    void resized() override;
    void paint(juce::Graphics& g) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cd::plugin::ui
