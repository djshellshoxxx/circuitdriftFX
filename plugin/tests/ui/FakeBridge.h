// UI test double for EditorBridge (coordinator-owned; shared by all UI snapshot apps).
// Owns a real CircuitDriftProcessor only for its parameter tree (no audio runs). Telemetry is a
// deterministic SYNTHETIC animation driven by advance(), so snapshots are reproducible.
#pragma once
#include <cmath>
#include <numbers>
#include "PluginProcessor.h"
#include "ui/EditorBridge.h"

namespace engine = cd::engine;
namespace cd::plugin::test {

class FakeBridge final : public EditorBridge {
public:
    explicit FakeBridge(Edition edition = Edition::Full)
        : processor_(edition, edition == Edition::Full ? "CircuitDrift" : "CircuitDrift LITE") {
        presets_ = {{"factory:clean-console", "Clean Console", "Factory", "Noise, leakage and faults off", true},
                    {"factory:mint-1968-console", "Mint 1968 Console", "Factory", "Fresh components", true},
                    {"user:my-scene", "My Scene", "User", "", false}};
        advance(0.0);
    }

    juce::AudioProcessor& processor() { return processor_; }
    // Deterministic synthetic telemetry at time t seconds.
    void advance(double t) {
        t_ = t;
        frame_ = {};
        frame_.generation = static_cast<std::uint64_t>(t * 30.0) + 1;
        frame_.sampleRate = 48000;
        frame_.sampleIndex = static_cast<std::int64_t>(t * 48000.0);
        frame_.latencySamples = 168;
        frame_.activeFactor = frame_.pendingFactor = 4;
        frame_.activeNewtonCap = 8;
        frame_.ringDecimation = 48;
        frame_.gainMatch = 1.0;
        for (int ch = 0; ch < engine::kMaxChannels; ++ch) {
            frame_.inPeak[ch] = 0.5 + 0.2 * std::sin(t * 3 + ch);
            frame_.inRms[ch] = frame_.inPeak[ch] * 0.5;
            frame_.outPeak[ch] = 0.6 + 0.2 * std::sin(t * 2.5 + ch);
            frame_.outRms[ch] = frame_.outPeak[ch] * 0.55;
        }
        for (int s = 0; s < engine::kSlots; ++s) {
            auto& st = frame_.slot[s];
            st.railNominalVolts = 300; st.railDcVolts = 300 - 12 * (0.5 + 0.5 * std::sin(t * 1.3 + s));
            st.railVolts = st.railDcVolts;
            for (int ch = 0; ch < engine::kMaxChannels; ++ch) {
                for (int c = 0; c < engine::kComponentCount; ++c) {
                    st.temperatureK[ch][c] = 298.15 + 10.0 * c * 0.5 + 5.0 * std::sin(t + c + ch);
                    st.powerW[ch][c] = 0.05 * (c + 1);
                }
                st.stageCurrentA[ch] = 1.2e-3; st.fluxWb[ch] = 0; st.fieldH[ch] = 0; st.fluxDensityT[ch] = 0;
            }
            for (int i = 0; i < engine::kRingSize; ++i) {
                const double ph = 2 * std::numbers::pi * i / engine::kRingSize;
                st.railRing[i] = st.railDcVolts + 4 * std::sin(ph * 3 + t);
                st.bhRing[i][0] = 80 * std::sin(ph + t);
                st.bhRing[i][1] = 0.9 * std::tanh(1.5 * std::sin(ph + t - 0.2));
            }
        }
        for (int i = 0; i < engine::kRingSize; ++i) {
            const double x = std::sin(2 * std::numbers::pi * i / 64.0 + t);
            frame_.transferRing[i][0] = 0.8 * x;
            frame_.transferRing[i][1] = std::tanh(1.6 * x) * 0.75;
        }
        frame_.transferWrite = 0;
    }

    // ---- EditorBridge ----
    juce::AudioProcessorValueTreeState& parameters() override { return processor_.parameters(); }
    juce::String slotParamId(const juce::String& baseName) const override {
        return (editSlot_ == 0 ? "slot.a." : "slot.b.") + baseName;
    }
    bool readTelemetry(engine::TelemetryFrame& out) override { out = frame_; return true; }
    StatusInfo status() override {
        StatusInfo s;
        s.lite = processor_.edition() == Edition::Lite;
        s.latencySamples = 168; s.sampleRate = 48000; s.activeOversampling = s.pendingOversampling = 4;
        s.keyState = "Manual C chromatic"; s.bpm = 120; s.networkStatus = "Off";
        s.communityStatus = "No catalog service configured (G06)";
        s.editSlot = editSlot_; s.presetName = "Clean Console"; s.presetModified = modified_;
        return s;
    }
    engine::EffectiveTargets effectiveTargets(int) override { return {}; }
    void beginGesture(const juce::String& label) override { pendingLabel_ = label; }
    void endGesture() override { if (pendingLabel_.isNotEmpty()) history_.entries.add(pendingLabel_); history_.position = history_.entries.size(); history_.canUndo = true; modified_ = true; }
    void undo() override { if (history_.position > 0) --history_.position; history_.canRedo = true; history_.canUndo = history_.position > 0; }
    void redo() override { if (history_.position < history_.entries.size()) ++history_.position; history_.canRedo = history_.position < history_.entries.size(); history_.canUndo = true; }
    HistoryInfo history() override { return history_; }
    void setEditSlot(int slot) override { editSlot_ = slot == 1 ? 1 : 0; }
    void copySlot(int, int) override { modified_ = true; }
    void swapSlots() override { modified_ = true; }
    std::vector<PresetEntry> listPresets(const juce::String& q) override {
        std::vector<PresetEntry> r;
        for (const auto& p : presets_) if (q.isEmpty() || p.name.containsIgnoreCase(q)) r.push_back(p);
        return r;
    }
    bool loadPreset(const juce::String&, int, juce::String&) override { modified_ = false; return true; }
    bool savePreset(const juce::String& name, juce::String& error) override {
        if (name.trim().isEmpty()) { error = "Name required"; return false; }
        presets_.push_back({"user:" + name, name, "User", "", false}); return true;
    }
    bool deletePreset(const juce::String&, juce::String&) override { return true; }
    bool importPreset(const juce::File&, juce::String&) override { return true; }
    bool exportPreset(const juce::String&, const juce::File&, juce::String&) override { return true; }
    void commitRestoration(int) override {}
    std::vector<FaultInfo> faults(int) override { return faults_; }
    bool applyFault(int, const FaultInfo& f, juce::String&) override { faults_.push_back(f); return true; }
    void clearFaults(int) override { faults_.clear(); }
    juce::String blueprintJson(int) override { return R"({"schemaVersion":1,"id":"factory","components":[]})"; }
    void requestBlueprintValidation(const juce::String&) override { reportReady_ = true; }
    bool blueprintReport(BlueprintReport& out) override {
        if (!reportReady_) return false;
        reportReady_ = false; out.ok = true; out.components = 17; out.nodes = 9; out.nonlinear = 2; out.unknowns = 12;
        return true;
    }
    bool applyBlueprint(int, const juce::String&, juce::String&) override { return true; }
    void resetEngine(bool) override {}
    bool startCapture(const CaptureRequest&, juce::String&) override { capture_.state = CaptureState::Recording; return true; }
    void submitCaptureFrame(const juce::Image&) override { ++capture_.framesWritten; }
    void stopCapture() override { capture_.state = CaptureState::Completed; }
    CaptureStatus captureStatus() override { auto c = capture_; c.mp4Reason = "Native MP4 encoder unavailable (G07)"; return c; }
    bool savePng(const juce::Image& image, const juce::File& file, juce::String& error) override {
        juce::PNGImageFormat png;
        juce::FileOutputStream out(file);
        if (!out.openedOk()) { error = "cannot open " + file.getFullPathName(); return false; }
        out.setPosition(0); out.truncate();
        return png.writeImageToStream(image, out);
    }
    UiSettings uiSettings() override { return settings_; }
    void setUiSettings(const UiSettings& s) override { settings_ = s; }

private:
    CircuitDriftProcessor processor_;
    engine::TelemetryFrame frame_{};
    double t_ = 0;
    int editSlot_ = 0;
    bool modified_ = false, reportReady_ = false;
    juce::String pendingLabel_;
    HistoryInfo history_;
    std::vector<PresetEntry> presets_;
    std::vector<FaultInfo> faults_;
    CaptureStatus capture_;
    UiSettings settings_;
};

// Renders a component at the given logical size and scale into an ARGB image (headless).
inline juce::Image renderComponent(juce::Component& c, int width, int height, float scale) {
    c.setBounds(0, 0, width, height);
    juce::Image img(juce::Image::ARGB, juce::roundToInt(width * scale), juce::roundToInt(height * scale), true);
    juce::Graphics g(img);
    g.addTransform(juce::AffineTransform::scale(scale));
    c.paintEntireComponent(g, false);
    return img;
}

inline bool writePng(const juce::Image& img, const juce::File& file) {
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    return out.openedOk() && png.writeImageToStream(img, out);
}

}  // namespace cd::plugin::test
