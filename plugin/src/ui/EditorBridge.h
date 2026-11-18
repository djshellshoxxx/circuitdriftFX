// CircuitDrift plugin: editor <-> processor contract (coordinator-owned; docs/ARCHITECTURE.md).
//
// The editor (UI agents) programs only against this interface. The processor adapter implements
// it; UI snapshot tests use a fake. All methods are MESSAGE-THREAD ONLY unless stated. Nothing
// here runs on the audio thread, and no method blocks on audio. Sound-changing actions are either
// APVTS parameter edits (host-visible) or explicit transactions below (one undo step each).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <vector>
#include "engine/EngineTypes.h"

namespace cd::plugin {

enum class EditorView : int { Easy = 0, Workbench = 1 };

// Per-instance UI preferences (saved with the session "ui" section; never automated).
struct UiSettings {
    float scale = 1.0f;          // logical scale 0.75..2.0 (independent of OS DPI)
    bool darkTheme = true;       // Dark brushed-metal / Light
    EditorView view = EditorView::Easy;
    bool educational = false;    // expanded educational tooltips
    bool reduceMotion = false;
    bool lowFrameRate = false;   // 30 FPS battery/low-motion option
    int overlay = 1;             // PCB overlay: 0 Off, 1 Thermal, 2 Voltage, 3 Signal
    bool xray = false;
    int lastWidth = 1000, lastHeight = 720;  // logical size
};

struct HistoryInfo {
    bool canUndo = false, canRedo = false;
    juce::String undoLabel, redoLabel;
    juce::StringArray entries;   // oldest first, completed transactions
    int position = 0;            // number of entries currently applied
};

struct PresetEntry {
    juce::String id;             // "factory:<slug>" or "user:<stem>"
    juce::String name, category, description;
    bool factory = false;
};

// Fault overlay request/state for one component (CD-F19). Types follow 03-blueprints spec.
enum class FaultType : int { None = 0, ElectrolyticOpen, ElectrolyticLeaky, CrackedResistor, MicrophonicTube };
struct FaultInfo {
    juce::String componentKey;   // engine::ComponentInfo::key, e.g. "C_k"
    FaultType type = FaultType::None;
    double severity = 0.0;       // 0..1
    bool enabled = false;
    std::uint64_t seed = 0;
    bool intermittent = false;   // cracked resistor only
    double modeHz = 800.0, modeQ = 8.0;  // microphonic only
};

struct BlueprintReport {
    bool ok = false;
    juce::StringArray diagnostics;   // "<componentId>.<pin>: message" or general text
    int components = 0, nodes = 0, nonlinear = 0, unknowns = 0;
    double estimatedCostNsPerSample = 0;
};

enum class CaptureFormat : int { Png = 0, Gif = 1, Mp4 = 2 };
enum class CaptureFraming : int { Portrait9x16 = 0, Square1x1 = 1, Landscape16x9 = 2 };
enum class CaptureState : int { Idle = 0, Armed, Recording, Finalizing, Completed, Cancelled, Failed };
struct CaptureRequest {
    CaptureFormat format = CaptureFormat::Gif;
    CaptureFraming framing = CaptureFraming::Portrait9x16;
    double seconds = 10.0;       // GIF <= 15 s, MP4 <= 60 s
    bool includeAudio = false;   // MP4 only: final plugin output tap
    bool loop = true;            // GIF
    juce::File destination;      // user-chosen local file
};
struct CaptureStatus {
    CaptureState state = CaptureState::Idle;
    double progress = 0.0;       // 0..1
    int framesWritten = 0, framesRepeated = 0;
    juce::String message;        // error/completion text
    bool mp4Available = false;   // false where no native encoder (G07)
    juce::String mp4Reason;
};

// Aggregated read-only status for badges/labels (cheap; poll at UI rate).
struct StatusInfo {
    bool lite = false;
    int latencySamples = 0;
    double sampleRate = 0;
    int activeOversampling = 0, pendingOversampling = 0;
    bool activeIir = false, pendingIir = false;
    bool reprepareRequired = false, qualityTransition = false;
    bool zeroLatency = false, pitchDriftActive = false, draftQuality = false;
    bool solverFallback = false, containmentActive = false, needsReset = false, bypassed = false;
    // Control (cd_control status)
    bool tempoFallback = false; double bpm = 120.0;
    juce::String keyState;       // e.g. "Manual C major", "MIDI A3", "Audio: low confidence"
    double pitchHz = 0, pitchConfidence = 0;
    bool harmonicAvailable[2] = {false, false};
    bool sidechainConnected = false;
    // Services
    juce::String networkStatus;  // e.g. "Off", "Joined: 3 members"
    juce::String communityStatus;// e.g. "No catalog service configured (G06)"
    // State
    int editSlot = 0;            // 0 = A, 1 = B (which slot the detailed controls edit)
    juce::String presetName;
    bool presetModified = false;
};

class EditorBridge {
public:
    virtual ~EditorBridge() = default;

    // Parameters: attach sliders/buttons with standard JUCE attachments (host-visible edits).
    virtual juce::AudioProcessorValueTreeState& parameters() = 0;
    // Registry string id of a slot base parameter for the current edit slot, e.g.
    // slotParamId("stage.drive") -> "slot.a.stage.drive" when editing A.
    virtual juce::String slotParamId(const juce::String& baseName) const = 0;

    // Live data (latest snapshot; false if none yet). Never blocks.
    virtual bool readTelemetry(engine::TelemetryFrame& out) = 0;
    virtual StatusInfo status() = 0;
    virtual engine::EffectiveTargets effectiveTargets(int slot) = 0;

    // Transactions and history (CD-F09). Gestures group parameter edits into one undo step.
    virtual void beginGesture(const juce::String& label) = 0;
    virtual void endGesture() = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual HistoryInfo history() = 0;

    // A/B (CD-F24).
    virtual void setEditSlot(int slot) = 0;
    virtual void copySlot(int from, int to) = 0;
    virtual void swapSlots() = 0;

    // Presets (local only; CD-F24).
    virtual std::vector<PresetEntry> listPresets(const juce::String& query) = 0;
    virtual bool loadPreset(const juce::String& id, int slot, juce::String& error) = 0;
    virtual bool savePreset(const juce::String& name, juce::String& error) = 0;
    virtual bool deletePreset(const juce::String& id, juce::String& error) = 0;
    virtual bool importPreset(const juce::File& file, juce::String& error) = 0;
    virtual bool exportPreset(const juce::String& id, const juce::File& file, juce::String& error) = 0;

    // Lifecycle timeline (CD-F18): scrubbing uses slot.<x>.lifecycle/scenario parameters;
    // committing Restoration changes replacement seeds once.
    virtual void commitRestoration(int slot) = 0;

    // Faults (CD-F19): explicit preview/apply; reversible; part of undo/A/B/preset state.
    virtual std::vector<FaultInfo> faults(int slot) = 0;
    virtual bool applyFault(int slot, const FaultInfo& fault, juce::String& error) = 0;
    virtual void clearFaults(int slot) = 0;

    // Blueprints (CD-F10): validation/compile runs on a worker; result polled.
    virtual juce::String blueprintJson(int slot) = 0;
    virtual void requestBlueprintValidation(const juce::String& json) = 0;
    virtual bool blueprintReport(BlueprintReport& out) = 0;  // true when a new report is ready
    virtual bool applyBlueprint(int slot, const juce::String& json, juce::String& error) = 0;

    // Engine reset (Warm/Cold) and re-prepare request after a staged quality change.
    virtual void resetEngine(bool cold) = 0;

    // Capture/export (CD-F03). The editor renders frames of its own canvas and submits them;
    // encoding and file writes happen on a worker.
    virtual bool startCapture(const CaptureRequest& request, juce::String& error) = 0;
    virtual void submitCaptureFrame(const juce::Image& frame) = 0;
    virtual void stopCapture() = 0;
    virtual CaptureStatus captureStatus() = 0;
    virtual bool savePng(const juce::Image& image, const juce::File& file, juce::String& error) = 0;

    // Per-instance UI settings.
    virtual UiSettings uiSettings() = 0;
    virtual void setUiSettings(const UiSettings& settings) = 0;
};

}  // namespace cd::plugin
