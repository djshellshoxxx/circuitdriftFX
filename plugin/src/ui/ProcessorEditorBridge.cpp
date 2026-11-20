#include "ui/ProcessorEditorBridge.h"
#include "PluginProcessor.h"
#include "ParameterLayout.h"
#include "core/ParameterRegistry.h"
#include <algorithm>
#include <cmath>

namespace cd::plugin {
namespace {
constexpr std::size_t kHistoryDepth = 20;
constexpr float kTolerance = 1.0e-6f;
bool sameValue(float a, float b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= kTolerance; }
bool unavailable(juce::String& error) {
    error = "Service not connected to the live processor (G11)";
    return false;
}
}  // namespace

juce::AudioProcessorValueTreeState& ProcessorEditorBridge::parameters() { return processor_.parameters(); }
juce::String ProcessorEditorBridge::slotParamId(const juce::String& name) const {
    return (editSlot_ == 0 ? "slot.a." : "slot.b.") + name;
}
bool ProcessorEditorBridge::readTelemetry(engine::TelemetryFrame& out) {
    return processor_.readEditorTelemetry(out);
}
StatusInfo ProcessorEditorBridge::status() {
    StatusInfo s;
    s.lite = processor_.edition() == Edition::Lite;
    s.channels = processor_.editorChannels();
    s.sampleRate = processor_.editorSampleRate();
    s.latencySamples = processor_.editorLatency();
    s.editSlot = editSlot_;
    s.networkStatus = "Off";
    s.communityStatus = "No catalog service configured (G06)";
    s.presetName = "Current session";
    s.presetModified = !steps_.empty();
    engine::TelemetryFrame frame{};
    if (processor_.readEditorTelemetry(frame)) {
        s.latencySamples = frame.latencySamples;
        s.sampleRate = frame.sampleRate;
        s.activeOversampling = frame.activeFactor;
        s.pendingOversampling = frame.pendingFactor;
        s.solverFallback = frame.solverFallback;
    }
    return s;
}
engine::EffectiveTargets ProcessorEditorBridge::effectiveTargets(int) { return {}; }

ProcessorEditorBridge::Snapshot ProcessorEditorBridge::snapshot() const {
    Snapshot result;
    result.reserve(core::kParamCount + 1);
    auto& tree = processor_.parameters();
    for (const auto& spec : core::kParams) {
        if (!isExposed(spec, processor_.edition())) continue;
        if (auto* p = tree.getParameter(spec.id)) result.emplace_back(spec.id, p->getValue());
    }
    if (processor_.edition() == Edition::Lite)
        if (auto* p = tree.getParameter(kBypassParamId)) result.emplace_back(kBypassParamId, p->getValue());
    return result;
}
void ProcessorEditorBridge::syncStateEpoch() {
    const auto epoch = processor_.editorStateEpoch();
    if (epoch == lastStateEpoch_) return;
    steps_.clear();
    position_ = 0;
    gestureOpen_ = false;
    gestureBefore_.clear();
    gestureLabel_.clear();
    lastStateEpoch_ = epoch;
}
void ProcessorEditorBridge::beginGesture(const juce::String& label) {
    syncStateEpoch();
    if (gestureOpen_) endGesture();
    gestureBefore_ = snapshot();
    gestureLabel_ = label.trim().isEmpty() ? juce::String("Edit parameters") : label;
    gestureOpen_ = true;
}
void ProcessorEditorBridge::endGesture() {
    syncStateEpoch();
    if (!gestureOpen_) return;
    gestureOpen_ = false;
    finish(gestureBefore_, gestureLabel_);
    gestureBefore_.clear();
    gestureLabel_.clear();
}
void ProcessorEditorBridge::finish(const Snapshot& before, const juce::String& label) {
    const auto after = snapshot();
    if (after.size() != before.size()) return; // the edition/parameter layout changed
    std::vector<Change> changes;
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (after[i].first != before[i].first) return;
        if (!sameValue(before[i].second, after[i].second))
            changes.push_back({after[i].first, before[i].second, after[i].second});
    }
    record(std::move(changes), label);
}
void ProcessorEditorBridge::record(std::vector<Change> changes, const juce::String& label) {
    if (changes.empty()) return;
    // Recording a completed edit truncates redo; at most twenty completed steps survive.
    steps_.erase(steps_.begin() + static_cast<std::ptrdiff_t>(position_), steps_.end());
    steps_.push_back({label, std::move(changes)});
    if (steps_.size() > kHistoryDepth) steps_.erase(steps_.begin());
    position_ = steps_.size();
}
bool ProcessorEditorBridge::setParameter(const juce::String& id, float normalized) {
    auto* p = processor_.parameters().getParameter(id);
    if (p == nullptr || !std::isfinite(normalized) || normalized < 0.0f || normalized > 1.0f) return false;
    if (sameValue(p->getValue(), normalized)) return true;
    p->beginChangeGesture();
    p->setValueNotifyingHost(normalized);
    p->endChangeGesture();
    return true;
}
bool ProcessorEditorBridge::replay(const Step& step, bool forward) {
    auto& tree = processor_.parameters();
    // An intervening host automation or session load must not be silently overwritten.
    // Preflight every touched parameter before publishing any undo/redo edit to the host.
    for (const auto& c : step.changes) {
        auto* p = tree.getParameter(c.id);
        if (p == nullptr || !sameValue(p->getValue(), forward ? c.before : c.after)) return false;
    }
    for (const auto& c : step.changes)
        if (!setParameter(c.id, forward ? c.after : c.before)) return false;
    return true;
}
void ProcessorEditorBridge::undo() {
    syncStateEpoch();
    if (gestureOpen_ || position_ == 0) return;
    if (replay(steps_[position_ - 1], false)) --position_;
    else { steps_.clear(); position_ = 0; }
}
void ProcessorEditorBridge::redo() {
    syncStateEpoch();
    if (gestureOpen_ || position_ >= steps_.size()) return;
    if (replay(steps_[position_], true)) ++position_;
    else { steps_.clear(); position_ = 0; }
}
HistoryInfo ProcessorEditorBridge::history() {
    syncStateEpoch();
    HistoryInfo info;
    info.canUndo = !gestureOpen_ && position_ > 0;
    info.canRedo = !gestureOpen_ && position_ < steps_.size();
    info.position = static_cast<int>(position_);
    if (info.canUndo) info.undoLabel = steps_[position_ - 1].label;
    if (info.canRedo) info.redoLabel = steps_[position_].label;
    for (const auto& step : steps_) info.entries.add(step.label);
    return info;
}
void ProcessorEditorBridge::setEditSlot(int slot) {
    if (slot == 0 || slot == 1) editSlot_ = slot;
}
void ProcessorEditorBridge::copySlot(int from, int to) {
    syncStateEpoch();
    if (processor_.edition() == Edition::Lite || from < 0 || from > 1 || to < 0 || to > 1 || from == to) return;
    if (gestureOpen_) endGesture();
    const auto before = snapshot();
    const juce::String source = from == 0 ? "slot.a." : "slot.b.";
    const juce::String target = to == 0 ? "slot.a." : "slot.b.";
    // Stage before writing so source/target values cannot alias during an operation.
    std::vector<std::pair<juce::String, float>> changes;
    for (const auto& item : before) {
        if (!item.first.startsWith(source)) continue;
        const auto id = target + item.first.substring(source.length());
        if (processor_.parameters().getParameter(id) == nullptr) return;
        changes.emplace_back(id, item.second);
    }
    for (const auto& item : changes) setParameter(item.first, item.second);
    finish(before, from == 0 ? "Copy A to B" : "Copy B to A");
}
void ProcessorEditorBridge::swapSlots() {
    syncStateEpoch();
    if (processor_.edition() == Edition::Lite) return;
    if (gestureOpen_) endGesture();
    const auto before = snapshot();
    std::vector<std::pair<juce::String, float>> changes;
    for (const auto& item : before) {
        if (!item.first.startsWith("slot.a.") && !item.first.startsWith("slot.b.")) continue;
        const auto counterpart = (item.first.startsWith("slot.a.") ? juce::String("slot.b.") : juce::String("slot.a.")) +
                                 item.first.substring(7);
        auto found = std::find_if(before.begin(), before.end(), [&](const auto& v) { return v.first == counterpart; });
        if (found == before.end()) return;
        changes.emplace_back(item.first, found->second);
    }
    for (const auto& item : changes) setParameter(item.first, item.second);
    // Mirroring the 0..1 crossfade preserves the audible blend when the slots exchange places.
    if (auto* p = processor_.parameters().getParameter("ab.crossfade"))
        setParameter("ab.crossfade", 1.0f - p->getValue());
    finish(before, "Swap A and B");
}

std::vector<PresetEntry> ProcessorEditorBridge::listPresets(const juce::String&) { return {}; }
bool ProcessorEditorBridge::loadPreset(const juce::String&, int, juce::String& error) { return unavailable(error); }
bool ProcessorEditorBridge::savePreset(const juce::String&, juce::String& error) { return unavailable(error); }
bool ProcessorEditorBridge::deletePreset(const juce::String&, juce::String& error) { return unavailable(error); }
bool ProcessorEditorBridge::importPreset(const juce::File&, juce::String& error) { return unavailable(error); }
bool ProcessorEditorBridge::exportPreset(const juce::String&, const juce::File&, juce::String& error) { return unavailable(error); }
void ProcessorEditorBridge::commitRestoration(int) {} // Requires runtime seed transaction; not yet wired.
std::vector<FaultInfo> ProcessorEditorBridge::faults(int) { return {}; }
bool ProcessorEditorBridge::applyFault(int, const FaultInfo&, juce::String& error) { return unavailable(error); }
void ProcessorEditorBridge::clearFaults(int) {} // No fault state installed.
juce::String ProcessorEditorBridge::blueprintJson(int) { return {}; }
void ProcessorEditorBridge::requestBlueprintValidation(const juce::String&) { blueprintReportPending_ = true; }
bool ProcessorEditorBridge::blueprintReport(BlueprintReport& out) {
    if (!blueprintReportPending_) return false;
    blueprintReportPending_ = false;
    out = {};
    out.diagnostics.add("Live blueprint compiler service is not connected (G11)");
    return true;
}
bool ProcessorEditorBridge::applyBlueprint(int, const juce::String&, juce::String& error) { return unavailable(error); }
void ProcessorEditorBridge::resetEngine(bool) {} // Never reset worker-owned state from the UI while audio is running.
bool ProcessorEditorBridge::startCapture(const CaptureRequest&, juce::String& error) { return unavailable(error); }
void ProcessorEditorBridge::submitCaptureFrame(const juce::Image&) {}
void ProcessorEditorBridge::stopCapture() {}
CaptureStatus ProcessorEditorBridge::captureStatus() {
    CaptureStatus s;
    s.mp4Reason = "Native MP4 encoder not available (G07)";
    s.message = "Capture service not connected (G11)";
    return s;
}
bool ProcessorEditorBridge::savePng(const juce::Image& image, const juce::File& file, juce::String& error) {
    if (!image.isValid()) { error = "Invalid screenshot"; return false; }
    juce::FileOutputStream output(file);
    if (!output.openedOk()) { error = "Cannot open screenshot destination"; return false; }
    juce::PNGImageFormat png;
    output.setPosition(0);
    output.truncate();
    if (!png.writeImageToStream(image, output)) {
        error = "Could not encode PNG image";
        return false;
    }
    output.flush();
    return true;
}
UiSettings ProcessorEditorBridge::uiSettings() { return ui_; }
void ProcessorEditorBridge::setUiSettings(const UiSettings& value) {
    ui_ = value;
    ui_.scale = std::isfinite(ui_.scale) ? std::clamp(ui_.scale, 0.75f, 2.0f) : 1.0f;
    if (ui_.view != EditorView::Easy && ui_.view != EditorView::Workbench) ui_.view = EditorView::Easy;
    ui_.overlay = std::clamp(ui_.overlay, 0, 3);
    ui_.pcbView = std::clamp(ui_.pcbView, 0, 2);
    ui_.lastWidth = std::clamp(ui_.lastWidth, 750, 4096);
    ui_.lastHeight = std::clamp(ui_.lastHeight, 540, 4096);
}
}  // namespace cd::plugin
