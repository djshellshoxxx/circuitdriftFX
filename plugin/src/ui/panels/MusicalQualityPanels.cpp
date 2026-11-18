// Musical (CD-F12..F16) and Quality (CD-F21/F25) panels.
#include <cmath>
#include "ui/panels/PanelCommon.h"

namespace cd::plugin::ui::pc {
namespace {

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

juce::String noteNameFromHz(double hz) {
    if (hz <= 0) return {};
    const int midi = juce::roundToInt(69.0 + 12.0 * std::log2(hz / 440.0));
    return juce::String(kNoteNames[((midi % 12) + 12) % 12]) + juce::String(midi / 12 - 1);
}

// ------------------------------------------------------------------------------------ Musical
class MusicalPanel final : public PanelBase {
public:
    explicit MusicalPanel(EditorBridge& b)
        : PanelBase(b),
          keyOn_(b, "Key lock enabled", "Quantise creative resonance and cutoff movement to the chosen key"),
          keySrc_(b, "Key source", "Where the key comes from: Manual, held MIDI note or detected audio pitch"),
          keyRoot_(b, "Root", "Root note of the scale"),
          keyScale_(b, "Scale", "Scale used for key lock; Custom uses the 12 note buttons"),
          tempoOn_(b, "Tempo sync enabled", "Follow the host tempo for recovery timing"),
          tempoDiv_(b, "Division", "Note length used for timing"),
          tempoMod_(b, "Modifier", "Straight, dotted or triplet timing"),
          tempoDepth_(b, "Tempo depth", "How strongly tempo shapes the sound"),
          hTarget_(b, "Harmonic target", "Balance of odd (left) and even (right) harmonics"),
          hAmount_(b, "Harmonic amount", "How far the harmonic balance is pushed"),
          transient_(b, "Transient preserve", "Keep attack transients clean while the body is driven"),
          scSource_(b, "Sidechain source", "Main input, external sidechain input or a blend"),
          scDepth_(b, "Sidechain depth", "How much the external signal loads the supply"),
          scHpf_(b, "Sidechain high-pass", "Filter low frequencies from the sidechain detector"),
          pitchDrift_(b, "Pitch drift", "Creative delay-based pitch movement; staged through the host latency lifecycle") {
        setTitle("Musical");
        heading("Key lock");
        keyOn_.bind("key.enabled");
        content_.addItem(keyOn_, 240, ParamToggle::kHeight, true);
        keySrc_.bind("key.source");
        keyRoot_.bind("key.root");
        keyScale_.bind("key.scale", {"Chromatic", "Major", "Natural minor", "Pentatonic major", "Pentatonic minor", "Custom"});
        content_.addItem(keySrc_, 160, ParamChoice::kHeight);
        content_.addItem(keyRoot_, 100, ParamChoice::kHeight);
        content_.addItem(keyScale_, 160, ParamChoice::kHeight);
        maskNote_ = note("", Tone::Dim);
        for (int i = 0; i < 12; ++i) {
            auto& bt = mask_[i];
            bt.setClickingTogglesState(true);
            bt.setButtonText(kNoteNames[i]);
            tip(bt, juce::String("Custom scale note ") + kNoteNames[i], "Allow this note in the custom scale");
            bt.onClick = [this, i] { toggleMask(i); };
            content_.addItem(bt, 46, 30);
        }
        keyStatus_ = note("", Tone::Normal);
        pitch_ = note("", Tone::Normal);

        heading("Tempo");
        tempoOn_.bind("tempo.enabled");
        content_.addItem(tempoOn_, 240, ParamToggle::kHeight, true);
        tempoDiv_.bind("tempo.division");
        tempoMod_.bind("tempo.modifier");
        tempoDepth_.bind("tempo.depth");
        content_.addItem(tempoDiv_, 120, ParamChoice::kHeight);
        content_.addItem(tempoMod_, 120, ParamChoice::kHeight);
        content_.addItem(tempoDepth_, 240, ParamSlider::kHeight, true);
        tempoStatus_ = note("", Tone::Normal);

        heading("Harmonics");
        hTarget_.bind("harmonics.target");
        hTarget_.setDisplay([](double v) {
            if (std::abs(v) < 0.01) return juce::String("Balanced");
            return juce::String(v < 0 ? "Odd " : "Even ") + juce::String(juce::roundToInt(std::abs(v) * 100.0)) + " %";
        });
        hAmount_.bind("harmonics.amount");
        content_.addItem(hTarget_, 240, ParamSlider::kHeight, true);
        content_.addItem(hAmount_, 240, ParamSlider::kHeight, true);
        harmStatus_ = note("", Tone::Warn);
        transient_.bind("transient.preserve");
        content_.addItem(transient_, 240, ParamSlider::kHeight, true);

        heading("Sidechain");
        scSource_.bind("sidechain.source");
        scDepth_.bind("sidechain.depth");
        scHpf_.bind("sidechain.hpf");
        content_.addItem(scSource_, 240, ParamChoice::kHeight, true);
        content_.addItem(scDepth_, 240, ParamSlider::kHeight, true);
        content_.addItem(scHpf_, 240, ParamSlider::kHeight, true);
        scStatus_ = note("", Tone::Normal);

        heading("Pitch drift");
        pitchDrift_.bind("pitchDrift.enabled");
        content_.addItem(pitchDrift_, 240, ParamToggle::kHeight, true);
        driftStatus_ = note("", Tone::Normal);
        note("Pitch drift is a creative effect, not an automatic consequence of supply sag. Changing it is staged: it takes effect when the host re-prepares and may change reported latency.");
        syncMask();
    }

    void refresh(const StatusInfo& s) override {
        bool ch = false;
        // Key
        const int scale = keyScale_.binding.bound() ? keyScale_.index() : 0;
        const bool custom = scale == 5;
        const int root = keyRoot_.binding.bound() ? std::max(0, keyRoot_.index()) : 0;
        for (int i = 0; i < 12; ++i) {
            const auto nm = juce::String(kNoteNames[(root + i) % 12]);
            if (mask_[i].getButtonText() != nm) { mask_[i].setButtonText(nm); mask_[i].setTitle("Custom scale note " + nm); }
            mask_[i].setTooltip("Allow " + nm + " (" + juce::String(i) + " semitones above the root) in the custom scale");
            mask_[i].setEnabled(custom && maskBinding_.bound());
        }
        syncMask();
        ch |= maskNote_->set(custom ? "Custom scale: toggle the notes allowed (at least one stays on). Buttons show note names from the root."
                                    : "Custom scale notes are available when Scale is set to Custom.",
                             Tone::Dim);
        ch |= keyStatus_->set("Key status: " + (s.keyState.isEmpty() ? juce::String("unknown") : s.keyState), Tone::Normal);
        juce::String ptxt;
        Tone ptone = Tone::Normal;
        if (s.pitchHz <= 0.0) { ptxt = "Detected pitch: none (needs audio input above -60 dBFS)"; ptone = Tone::Dim; }
        else if (s.pitchConfidence < 0.8) {
            ptxt = "Detected pitch: low confidence (" + juce::String(juce::roundToInt(s.pitchConfidence * 100.0)) + " %), roughly " + juce::String(s.pitchHz, 0) + " Hz. Not used as certain.";
            ptone = Tone::Warn;
        } else {
            ptxt = "Detected pitch: " + juce::String(s.pitchHz, 1) + " Hz (" + noteNameFromHz(s.pitchHz) + "), confidence " + juce::String(juce::roundToInt(s.pitchConfidence * 100.0)) + " %";
        }
        ch |= pitch_->set(ptxt, ptone);
        // Tempo
        ch |= tempoStatus_->set(s.tempoFallback ? "120 BPM assumed: the host provides no tempo, so a fixed fallback is used."
                                                : "Host tempo: " + juce::String(s.bpm, 1) + " BPM",
                                s.tempoFallback ? Tone::Warn : Tone::Normal);
        // Harmonics
        const bool harm = s.harmonicAvailable[juce::jlimit(0, 1, s.editSlot)];
        hTarget_.setEnabled(harm);
        hAmount_.setEnabled(harm);
        ch |= harmStatus_->set(harm ? "" : "Harmonic balance is unavailable: the current circuit does not expose a usable harmonic map, so these two controls have no effect.", Tone::Warn);
        // Sidechain
        const int src = scSource_.binding.bound() ? scSource_.index() : 0;
        juce::String sct = s.sidechainConnected ? "External sidechain: connected" : "External sidechain: not connected";
        if (src == 0) sct += " (source is Main, external input unused)";
        else if (!s.sidechainConnected) sct += ". Missing external input adds zero load; the main signal keeps working.";
        ch |= scStatus_->set(sct, (src != 0 && !s.sidechainConnected) ? Tone::Warn : Tone::Normal);
        // Pitch drift
        const bool want = pitchDrift_.binding.bound() && pitchDrift_.getToggleState();
        juce::String dt = juce::String("Active: ") + (s.pitchDriftActive ? "on" : "off");
        if (s.zeroLatency) { dt += ". Unavailable in Zero Latency mode."; pitchDrift_.setEnabled(false); }
        else {
            pitchDrift_.setEnabled(pitchDrift_.binding.bound());
            if (want != s.pitchDriftActive) dt += ". Pending: " + juce::String(want ? "on" : "off") + ", applies after the host re-prepares (latency may change).";
            else if (s.pitchDriftActive) dt += ". Adds about 1.5 ms base delay to the reported latency.";
        }
        ch |= driftStatus_->set(dt, (want != s.pitchDriftActive) ? Tone::Warn : Tone::Normal);
        if (ch) relayout();
    }

private:
    void syncMask() {
        if (!maskBinding_.bound()) maskBinding_.bind("key.mask", [this](float) { syncMaskButtons(); });
        syncMaskButtons();
    }
    void syncMaskButtons() {
        const int m = maskBinding_.bound() ? static_cast<int>(std::lround(maskBinding_.value())) : 4095;
        for (int i = 0; i < 12; ++i) mask_[i].setToggleState((m >> i) & 1, juce::dontSendNotification);
    }
    void toggleMask(int i) {
        if (!maskBinding_.bound()) return;
        int m = static_cast<int>(std::lround(maskBinding_.value()));
        m ^= (1 << i);
        if ((m & 4095) == 0) { syncMaskButtons(); return; }  // a custom scale needs at least one note
        maskBinding_.setComplete(static_cast<float>(m & 4095));
    }

    ParamToggle keyOn_;
    ParamChoice keySrc_, keyRoot_, keyScale_;
    ParamToggle tempoOn_;
    ParamChoice tempoDiv_, tempoMod_;
    ParamSlider tempoDepth_, hTarget_, hAmount_, transient_;
    ParamChoice scSource_;
    ParamSlider scDepth_, scHpf_;
    ParamToggle pitchDrift_;
    juce::TextButton mask_[12];
    ParamBinding maskBinding_{bridge, "Edit custom key mask"};
    NoteLabel *maskNote_ = nullptr, *keyStatus_ = nullptr, *pitch_ = nullptr, *tempoStatus_ = nullptr, *harmStatus_ = nullptr, *scStatus_ = nullptr, *driftStatus_ = nullptr;
};

// ------------------------------------------------------------------------------------ Quality
juce::String osName(int f) { return f <= 1 ? "Off (1x)" : juce::String(f) + "x"; }

class QualityPanel final : public PanelBase {
public:
    explicit QualityPanel(EditorBridge& b)
        : PanelBase(b),
          mode_(b, "Quality mode", "Eco, Studio or Render processing quality"),
          os_(b, "Oversampling", "Oversampling of the whole nonlinear circuit; Off (1x) and 2x are Draft quality"),
          phase_(b, "Filter phase", "FIR is linear phase with more latency; IIR has low latency"),
          zero_(b, "Zero latency", "Report no latency: 1x Draft antialiasing, no pitch drift"),
          soft_(b, "Output soft clip", "Smooth output ceiling just below full scale; separate from emergency containment"),
          hpf_(b, "Input high-pass", "Remove subsonic content before the circuit") {
        setTitle("Quality");
        heading("Quality and latency");
        mode_.bind("quality.mode");
        content_.addItem(mode_, 160, ParamChoice::kHeight);
        os_.bind("quality.oversampling", {"Auto", "Off (1x) - Draft", "2x - Draft", "4x", "8x", "16x"});
        content_.addItem(os_, 160, ParamChoice::kHeight);
        phase_.bind("quality.phase", {"FIR (linear phase)", "IIR (low latency)"});
        content_.addItem(phase_, 160, ParamChoice::kHeight);
        zero_.bind("quality.zeroLatency");
        content_.addItem(zero_, 240, ParamToggle::kHeight, true);
        draft_ = note("", Tone::Warn);
        zeroNote_ = note("", Tone::Warn);
        heading("Active and pending");
        active_ = note("", Tone::Normal);
        pending_ = note("", Tone::Normal);
        latency_ = note("", Tone::Normal);
        note("Quality changes are prepared off-thread and applied after the host re-prepares the plugin (stop and restart playback). Reported latency changes only then; no change happens mid-buffer.");
        heading("Output protection");
        soft_.bind("output.softClip");
        hpf_.bind("hpf.enabled");
        content_.addItem(soft_, 200, ParamToggle::kHeight);
        content_.addItem(hpf_, 200, ParamToggle::kHeight);
        protect_ = note("", Tone::Normal);
        heading("Reset");
        warm_ = makeButton("Reset Warm", "Clear audio and filter state but keep the thermal state of the circuit");
        cold_ = makeButton("Reset Cold", "Full cold start: clears audio, thermal and supply state");
        warm_->onClick = [this] { bridge.resetEngine(false); };
        cold_->onClick = [this] { bridge.resetEngine(true); };
        content_.addItem(*warm_, buttonBasis("Reset Warm"), 30);
        content_.addItem(*cold_, buttonBasis("Reset Cold"), 30);
        note("Warm keeps heat and rail state; Cold behaves like switching the unit on from room temperature.");
    }

    void refresh(const StatusInfo& s) override {
        bool ch = false;
        const double sr = s.sampleRate;
        auto lat = [sr](int samples) { return juce::String(samples) + " samples" + (sr > 0 ? " (" + juce::String(samples * 1000.0 / sr, 2) + " ms)" : juce::String()); };
        const bool draftNow = s.draftQuality || s.activeOversampling <= 2 || s.zeroLatency;
        ch |= draft_->set(draftNow ? "Draft quality: Off (1x) and 2x oversampling trade aliasing performance for speed or latency. FIR and 4x or more is the normal quality." : "", Tone::Warn);
        ch |= zeroNote_->set(zero_.getToggleState() || s.zeroLatency
                                 ? "Zero Latency: Off (1x) Draft antialiasing, no lookahead. Unavailable meanwhile: pitch drift, audio cross-talk delay, high-quality latency padding."
                                 : "",
                             Tone::Warn);
        ch |= active_->set("Active now: " + osName(s.activeOversampling) + ", " + (s.activeIir ? "IIR" : "FIR") + (s.zeroLatency ? ", zero latency" : "") + (s.lite ? " (LITE edition)" : ""), Tone::Normal);
        const bool differs = s.pendingOversampling != s.activeOversampling || s.pendingIir != s.activeIir;
        ch |= pending_->set(differs || s.reprepareRequired
                                ? "Pending: " + osName(s.pendingOversampling) + ", " + (s.pendingIir ? "IIR" : "FIR") + ". Applies after re-prepare."
                                : "Pending: none, active settings match the selection.",
                            (differs || s.reprepareRequired) ? Tone::Warn : Tone::Dim);
        ch |= latency_->set("Reported latency: " + lat(s.latencySamples) + (s.qualityTransition ? ". Quality transition in progress." : ""), Tone::Normal);
        juce::String p = juce::String("Soft clip: ") + (soft_.getToggleState() ? "on" : "off (unclipped float output)") + ". Input high-pass: " + (hpf_.getToggleState() ? "on" : "off") + ".";
        p += s.containmentActive ? " Emergency containment is ACTIVE." : " Emergency containment idle (always armed).";
        if (s.solverFallback) p += " Solver fallback engaged.";
        if (s.needsReset) p += " A reset is recommended.";
        ch |= protect_->set(p, (s.containmentActive || s.solverFallback || s.needsReset) ? Tone::Warn : Tone::Normal);
        if (ch) relayout();
    }

private:
    ParamChoice mode_, os_, phase_;
    ParamToggle zero_, soft_, hpf_;
    NoteLabel *draft_ = nullptr, *zeroNote_ = nullptr, *active_ = nullptr, *pending_ = nullptr, *latency_ = nullptr, *protect_ = nullptr;
    std::unique_ptr<juce::TextButton> warm_, cold_;
};

}  // namespace

std::unique_ptr<Panel> makeMusicalPanel(EditorBridge& b) { return std::make_unique<MusicalPanel>(b); }
std::unique_ptr<Panel> makeQualityPanel(EditorBridge& b) { return std::make_unique<QualityPanel>(b); }

}  // namespace cd::plugin::ui::pc
