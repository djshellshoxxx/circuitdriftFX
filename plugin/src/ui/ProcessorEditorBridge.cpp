#include "ui/ProcessorEditorBridge.h"
#include "ui/BlueprintValidationService.h"
#include "PluginProcessor.h"
#include "ParameterLayout.h"
#include "core/ParameterRegistry.h"
#include <filesystem>
#include <string>
#include <utility>
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
std::filesystem::path nativePath(const juce::File& file) {
    // JUCE paths are Unicode. u8path handles non-ASCII user profiles on Windows.
    return std::filesystem::u8path(file.getFullPathName().toStdString());
}
std::filesystem::path defaultPresetDirectory() {
    return nativePath(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("CircuitDrift").getChildFile("Presets"));
}
juce::String displayUtf8(const std::string& str) {
    return juce::String::fromUTF8(str.data(), static_cast<int>(str.size()));
}
}  // namespace

ProcessorEditorBridge::ProcessorEditorBridge(CircuitDriftProcessor& processor)
    : ProcessorEditorBridge(processor, defaultPresetDirectory()) {}
ProcessorEditorBridge::ProcessorEditorBridge(CircuitDriftProcessor& processor,
                                             std::filesystem::path userPresetDirectory)
    : processor_(processor), presetLibrary_(std::move(userPresetDirectory)) {}
ProcessorEditorBridge::~ProcessorEditorBridge() = default;

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
    CircuitDriftProcessor::EditorSnapshot snapshot{};
    if (processor_.readEditorSnapshot(snapshot)) {
        const auto& frame = snapshot.telemetry;
        const auto& controls = snapshot.controls;
        s.latencySamples = frame.latencySamples;
        s.sampleRate = frame.sampleRate;
        s.activeOversampling = frame.activeFactor;
        s.pendingOversampling = frame.pendingFactor;
        s.activeIir = frame.activeFamilyIir != 0;
        s.pendingIir = frame.pendingFamilyIir != 0;
        s.reprepareRequired = frame.reprepareRequired;
        s.qualityTransition = frame.qualityTransition;
        s.zeroLatency = frame.activeZeroLatency;
        s.pitchDriftActive = frame.activePitchDrift && !frame.activeZeroLatency;
        s.draftQuality = frame.activeFactor <= 2 || frame.activeZeroLatency;
        s.solverFallback = frame.solverFallback;
        s.needsReset = frame.needsReset;
        s.bypassed = frame.bypassed;
        s.tempoFallback = controls.tempoFallback;
        s.bpm = snapshot.bpm;
        s.pitchHz = controls.pitchHz;
        s.pitchConfidence = controls.pitchConfidence;
        s.sidechainConnected = controls.sidechainConnected;
        for (int slot = 0; slot < engine::kSlots; ++slot)
            s.harmonicAvailable[slot] = controls.harmonicAvailable[slot];
        switch (controls.keyState) {
            case control::KeyState::Off: s.keyState = "Off"; break;
            case control::KeyState::ManualReference: s.keyState = "Manual reference"; break;
            case control::KeyState::NoteActive: s.keyState = "Note active"; break;
            case control::KeyState::NoteHold: s.keyState = "Last note held"; break;
            case control::KeyState::Returning: s.keyState = "Returning to reference"; break;
        }
        if (controls.pitchDriftHeld) s.keyState += "; pitch drift held";
    }
    return s;
}
engine::EffectiveTargets ProcessorEditorBridge::effectiveTargets(int slot) {
    if (slot < 0 || slot >= engine::kSlots) return {};
    CircuitDriftProcessor::EditorSnapshot snapshot{};
    return processor_.readEditorSnapshot(snapshot) ? snapshot.targets[slot] : engine::EffectiveTargets{};
}

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
    gestureBefore_.clear();
    gestureLabel_ = label.trim().isEmpty() ? juce::String("Edit parameters") : label;
    gestureOpen_ = true;
}
void ProcessorEditorBridge::touchParameter(const juce::String& id) {
    syncStateEpoch();
    if (!gestureOpen_ || std::any_of(gestureBefore_.begin(), gestureBefore_.end(),
                                  [&](const auto& value) { return value.first == id; })) return;
    if (auto* parameter = processor_.parameters().getParameter(id))
        gestureBefore_.emplace_back(id, parameter->getValue());
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
    std::vector<Change> changes;
    for (const auto& [id, initial] : before) {
        auto* parameter = processor_.parameters().getParameter(id);
        if (parameter == nullptr) return;
        const auto current = parameter->getValue();
        if (!sameValue(initial, current)) changes.push_back({id, initial, current});
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
    beginGesture(from == 0 ? "Copy A to B" : "Copy B to A");
    for (const auto& item : changes) {
        touchParameter(item.first);
        setParameter(item.first, item.second);
    }
    endGesture();
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
    // Stage the crossfade with both slots, before host notifications can change parameters.
    for (const auto& item : before)
        if (item.first == "ab.crossfade") changes.emplace_back(item.first, 1.0f - item.second);
    beginGesture("Swap A and B");
    for (const auto& item : changes) {
        touchParameter(item.first);
        setParameter(item.first, item.second);
    }
    endGesture();
}

std::vector<PresetEntry> ProcessorEditorBridge::listPresets(const juce::String& query) {
    // Local, bounded directory scan on the message thread; no audio ownership.
    std::vector<PresetEntry> entries;
    state::PresetListReport report;
    for (const auto& preset : presetLibrary_.list(query.toStdString(), &report)) {
        PresetEntry entry;
        entry.id = displayUtf8(preset.id);
        entry.name = displayUtf8(preset.title);
        entry.factory = preset.factory;
        entry.category = preset.factory ? "Factory" : preset.valid ? "User" : "Unreadable user preset";
        entry.description = preset.valid ? displayUtf8(preset.author)
                                         : "Unreadable preset: " + displayUtf8(preset.error);
        entries.push_back(std::move(entry));
    }
    catalogMessage_.clear();
    if (report.truncated)
        catalogMessage_ = "Catalog scan limit reached; some presets may be missing.";
    if (!report.error.empty()) {
        if (catalogMessage_.isNotEmpty()) catalogMessage_ += " ";
        catalogMessage_ += "Could not finish reading the preset directory: " + displayUtf8(report.error);
    }
    return entries;
}
// Applying or capturing a sound requires atomic slot seed/blueprint transactions.
// Do not pretend that APVTS-only changes have loaded or saved a complete scene.
bool ProcessorEditorBridge::loadPreset(const juce::String&, int, juce::String& error) { return unavailable(error); }
bool ProcessorEditorBridge::savePreset(const juce::String&, juce::String& error) { return unavailable(error); }
bool ProcessorEditorBridge::deletePreset(const juce::String& id, juce::String& error) {
    std::string why;
    if (!presetLibrary_.remove(id.toStdString(), why)) { error = displayUtf8(why); return false; }
    error.clear();
    return true;
}
bool ProcessorEditorBridge::importPreset(const juce::File& file, juce::String& error) {
    if (file.getFullPathName().isEmpty()) { error = "Select a preset file"; return false; }
    std::string id, why;
    if (!presetLibrary_.importFile(nativePath(file), {}, false, id, why)) {
        error = displayUtf8(why);
        return false;
    }
    error.clear();
    return true;
}
bool ProcessorEditorBridge::exportPreset(const juce::String& id, const juce::File& file, juce::String& error) {
    if (file.getFullPathName().isEmpty()) { error = "Select a destination"; return false; }
    std::string why;
    if (!presetLibrary_.exportPreset(id.toStdString(), nativePath(file), why)) {
        error = displayUtf8(why);
        return false;
    }
    error.clear();
    return true;
}
void ProcessorEditorBridge::commitRestoration(int) {} // Requires runtime seed transaction; not yet wired.
std::vector<FaultInfo> ProcessorEditorBridge::faults(int) { return {}; }
bool ProcessorEditorBridge::applyFault(int, const FaultInfo&, juce::String& error) { return unavailable(error); }
void ProcessorEditorBridge::clearFaults(int) {} // No fault state installed.
juce::String ProcessorEditorBridge::blueprintJson(int) { return {}; }
ProcessorEditorBridge::ValidationContext ProcessorEditorBridge::validationContext() {
    ValidationContext context;
    context.stateEpoch = processor_.editorStateEpoch();
    const auto sampleRate = processor_.editorSampleRate();
    if (sampleRate > 0) context.sampleRate = sampleRate;
    CircuitDriftProcessor::EditorSnapshot snapshot;
    if (processor_.readEditorSnapshot(snapshot)) {
        context.sampleRate = snapshot.telemetry.sampleRate;
        context.oversampling = snapshot.telemetry.activeFactor;
        context.preview = false;
    }
    return context;
}
void ProcessorEditorBridge::requestBlueprintValidation(const juce::String& json) {
    if (!blueprintValidator_) blueprintValidator_ = std::make_unique<BlueprintValidationService>();
    validationContext_ = validationContext();
    if (processor_.edition() == Edition::Lite) {
        blueprintValidator_->reject("Blueprint validation is available in the Full edition.");
    } else if (json.getNumBytesAsUTF8() > blueprint::kMaxBytes) {
        blueprintValidator_->reject("Blueprint exceeds the 1 MiB UTF-8 limit.");
    } else {
        blueprintValidator_->request(json.toStdString(),
            {validationContext_.sampleRate, validationContext_.oversampling, 1.0});
    }
}
void ProcessorEditorBridge::cancelBlueprintValidation() {
    if (blueprintValidator_) blueprintValidator_->cancel();
}
bool ProcessorEditorBridge::blueprintReport(BlueprintReport& out) {
    if (!blueprintValidator_ || !blueprintValidator_->take(out)) return false;
    out.previewConfiguration = validationContext_.preview;
    if (!(validationContext_ == validationContext())) {
        out.ok = false;
        out.diagnostics.add("The audio configuration or host session changed. Validate again.");
    }
    return true;
}
bool ProcessorEditorBridge::applyBlueprint(int, const juce::String&, juce::String& error) { return unavailable(error); }
void ProcessorEditorBridge::resetEngine(bool cold) {
    if (supportsEngineReset()) processor_.requestEngineReset(cold);
}
bool ProcessorEditorBridge::supportsEngineReset() const {
    return processor_.edition() == Edition::Full && processor_.editorSampleRate() > 0.0;
}
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
    if (file.getFullPathName().isEmpty()) { error = "Select a screenshot destination"; return false; }
    // Encode before opening the destination. Failed encoding, flush, or rename
    // must never truncate an existing screenshot. The existing atomic local-file
    // writer also accepts binary strings (including embedded NUL bytes).
    juce::MemoryOutputStream encoded;
    juce::PNGImageFormat png;
    if (!png.writeImageToStream(image, encoded) || encoded.getDataSize() == 0) {
        error = "Could not encode PNG image";
        return false;
    }
    const std::string bytes(static_cast<const char*>(encoded.getData()), encoded.getDataSize());
    std::string why;
    if (!state::saveStateFileAtomic(file.getFullPathName().toStdString(), bytes, why)) {
        error = "Cannot save PNG: " + displayUtf8(why);
        return false;
    }
    error.clear();
    return true;
}
UiSettings ProcessorEditorBridge::uiSettings() { return processor_.editorSettings(); }
void ProcessorEditorBridge::setUiSettings(const UiSettings& value) {
    auto bounded = value;
    bounded.scale = std::isfinite(bounded.scale) ? std::clamp(bounded.scale, 0.75f, 2.0f) : 1.0f;
    if (bounded.view != EditorView::Easy && bounded.view != EditorView::Workbench) bounded.view = EditorView::Easy;
    bounded.overlay = std::clamp(bounded.overlay, 0, 3);
    bounded.pcbView = std::clamp(bounded.pcbView, 0, 2);
    bounded.lastWidth = std::clamp(bounded.lastWidth, 750, 4096);
    bounded.lastHeight = std::clamp(bounded.lastHeight, 540, 4096);
    processor_.setEditorSettings(bounded);
}
}  // namespace cd::plugin
