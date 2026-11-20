// Live message-thread adapter for the CircuitDrift editor.
// Only parameter transactions and read-only telemetry are wired in this milestone.
// Unimplemented services fail explicitly; never substitute synthetic FakeBridge data.
#pragma once
#include "ui/EditorBridge.h"
#include <cstddef>
#include <utility>
#include <vector>

namespace cd::plugin {
class CircuitDriftProcessor;

class ProcessorEditorBridge final : public EditorBridge {
public:
    explicit ProcessorEditorBridge(CircuitDriftProcessor& processor) : processor_(processor) {}

    juce::AudioProcessorValueTreeState& parameters() override;
    juce::String slotParamId(const juce::String& baseName) const override;
    bool readTelemetry(engine::TelemetryFrame& out) override;
    StatusInfo status() override;
    engine::EffectiveTargets effectiveTargets(int slot) override;

    void beginGesture(const juce::String& label) override;
    void endGesture() override;
    void undo() override;
    void redo() override;
    HistoryInfo history() override;
    void setEditSlot(int slot) override;
    void copySlot(int from, int to) override;
    void swapSlots() override;

    std::vector<PresetEntry> listPresets(const juce::String& query) override;
    bool loadPreset(const juce::String& id, int slot, juce::String& error) override;
    bool savePreset(const juce::String& name, juce::String& error) override;
    bool deletePreset(const juce::String& id, juce::String& error) override;
    bool importPreset(const juce::File& file, juce::String& error) override;
    bool exportPreset(const juce::String& id, const juce::File& file, juce::String& error) override;
    void commitRestoration(int slot) override;
    bool supportsRestorationCommit() const override { return false; }
    std::vector<FaultInfo> faults(int slot) override;
    bool applyFault(int slot, const FaultInfo& fault, juce::String& error) override;
    void clearFaults(int slot) override;
    juce::String blueprintJson(int slot) override;
    void requestBlueprintValidation(const juce::String& json) override;
    bool blueprintReport(BlueprintReport& out) override;
    bool applyBlueprint(int slot, const juce::String& json, juce::String& error) override;
    void resetEngine(bool cold) override;
    bool startCapture(const CaptureRequest& request, juce::String& error) override;
    void submitCaptureFrame(const juce::Image& frame) override;
    void stopCapture() override;
    CaptureStatus captureStatus() override;
    bool savePng(const juce::Image& image, const juce::File& file, juce::String& error) override;
    UiSettings uiSettings() override;
    void setUiSettings(const UiSettings& settings) override;

private:
    struct Change { juce::String id; float before = 0, after = 0; };
    struct Step { juce::String label; std::vector<Change> changes; };
    using Snapshot = std::vector<std::pair<juce::String, float>>;

    Snapshot snapshot() const;
    void finish(const Snapshot& before, const juce::String& label);
    void record(std::vector<Change> changes, const juce::String& label);
    bool replay(const Step& step, bool forward);
    bool setParameter(const juce::String& id, float normalized);
    void syncStateEpoch();

    CircuitDriftProcessor& processor_;
    UiSettings ui_{};
    int editSlot_ = 0;
    bool gestureOpen_ = false;
    Snapshot gestureBefore_;
    juce::String gestureLabel_;
    std::vector<Step> steps_;
    std::size_t position_ = 0;
    bool blueprintReportPending_ = false;
    std::uint64_t lastStateEpoch_ = 0;
};
}  // namespace cd::plugin
