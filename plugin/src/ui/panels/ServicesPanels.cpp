// Capture (CD-F03), Network (CD-F11) and Community (CD-F04) panels.
#include <cmath>
#include <cstdint>
#include "ui/panels/CapturePanelTestHooks.h"
#include "ui/panels/PanelCommon.h"
#include "ui/shell/EditorCore.h"
#include "ui/workbench/GestureMath.h"

namespace cd::plugin::ui::pc {
namespace {

// ------------------------------------------------------------------------------------ Capture
class CapturePanel final : public PanelBase {
public:
    explicit CapturePanel(EditorBridge& b)
        : PanelBase(b),
          framing_(b, "Framing", "Aspect ratio of the captured canvas"),
          duration_(b, "Duration", "Recording length; GIF up to 15 s, MP4 up to 60 s"),
          audio_(b, "Include audio", "MP4 only: add the final plugin output as stereo AAC"),
          loop_(b, "Loop GIF", "GIF only: repeat the animation forever"),
          png_("PNG"), gif_("GIF"), mp4_("MP4") {
        setTitle("Capture");
        heading("Capture");
        note("Captures only CircuitDrift's own visuals, never the desktop or other windows. Files are written locally.", Tone::Dim);
        heading("Format");
        int id = 0;
        for (auto* t : {&png_, &gif_, &mp4_}) {
            t->setClickingTogglesState(true);
            t->setRadioGroupId(7201);
            const int fmt = id++;
            t->onClick = [this, fmt] { if (fmtButton(fmt).getToggleState()) setFormat(static_cast<CaptureFormat>(fmt)); };
        }
        tip(png_, "PNG screenshot", "Save one still image of the canvas");
        tip(gif_, "GIF animation", "Record an animated GIF, up to 15 seconds, 20 frames per second, no audio");
        tip(mp4_, "MP4 video", "Record H.264 video with optional audio, up to 60 seconds");
        content_.addItem(png_, 80, 32);
        content_.addItem(gif_, 80, 32);
        content_.addItem(mp4_, 80, 32);
        mp4Reason_ = note("", Tone::Warn);
        framing_.box.addItem("9:16 portrait", 1);
        framing_.box.addItem("1:1 square", 2);
        framing_.box.addItem("16:9 landscape", 3);
        framing_.box.setSelectedId(1, juce::dontSendNotification);
        content_.addItem(framing_, 240, ParamChoice::kHeight, true);
        duration_.slider.setRange(1.0, 15.0, 1.0);
        duration_.slider.setValue(10.0, juce::dontSendNotification);
        duration_.slider.onValueChange = [this] { clearActionResult(); };
        duration_.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v)) + " s"; };
        duration_.slider.valueFromTextFunction = [this](const juce::String& t) {
            return gesture::parseBoundedQuantity(t.toStdString(), "s", duration_.slider.getMinimum(), duration_.slider.getMaximum())
                .value_or(duration_.slider.getValue());
        };
        duration_.slider.updateText();
        content_.addItem(duration_, 240, ParamSlider::kHeight, true);
        durNote_ = note("", Tone::Dim);
        loop_.setToggleState(true, juce::dontSendNotification);
        framing_.box.onChange = [this] { clearActionResult(); };
        audio_.onClick = [this] { clearActionResult(); };
        loop_.onClick = [this] { clearActionResult(); };
        content_.addItem(audio_, 200, ParamToggle::kHeight);
        content_.addItem(loop_, 200, ParamToggle::kHeight);
        heading("Destination");
        dest_ = makeButton("Choose destination...", "Choose where the capture file is written");
        dest_->onClick = [this] { chooseDestination(); };
        content_.addItem(*dest_, buttonBasis("Choose destination..."), 30);
        destNote_ = note("", Tone::Dim);
        destNote_->setTitle("Capture destination");
        heading("Record");
        record_ = makeButton("Record", "Start recording the canvas to the chosen destination");
        stop_ = makeButton("Stop", "Stop recording and finish the file");
        record_->onClick = [this] { startRecording(); };
        stop_->onClick = [this] { clearActionResult(); bridge.stopCapture(); refresh(bridge.status()); };
        content_.addItem(*record_, buttonBasis("Save PNG"), 32);
        content_.addItem(*stop_, buttonBasis("Stop"), 32);
        progress_.reset(new juce::ProgressBar(progressValue_));
        tip(*progress_, "Capture progress", "Progress of the current capture");
        content_.addItem(*progress_, 240, 22, true);
        state_ = note("", Tone::Normal);
        qualityWarning_ = note("", Tone::Warn);
        msg_ = note("", Tone::Normal);
        state_->setTitle("Capture state");
        qualityWarning_->setTitle("Capture quality warning");
        msg_->setTitle("Capture result");
        note("PNG captures the current canvas at the chosen aspect. GIF: 20 FPS, 256 colours, no audio. MP4: H.264 30 FPS, optional stereo AAC. A format is never silently swapped for another.");
        setFormat(CaptureFormat::Gif);
        gif_.setToggleState(true, juce::dontSendNotification);
        refresh(bridge.status());
    }
    void refresh(const StatusInfo&) override {
        const auto cs = bridge.captureStatus();
        bool ch = false;
        ch |= mp4Reason_->set(cs.mp4Available ? "" : "MP4 disabled: " + (cs.mp4Reason.isEmpty() ? juce::String("no native encoder available") : cs.mp4Reason) + ". Use GIF or PNG instead.", Tone::Warn);
        static const char* const names[] = {"Idle", "Armed", "Recording", "Finalizing", "Completed", "Cancelled", "Failed"};
        const int si = juce::jlimit(0, 6, static_cast<int>(cs.state));
        const bool busy = cs.state == CaptureState::Armed || cs.state == CaptureState::Recording || cs.state == CaptureState::Finalizing;
        ch |= state_->set(juce::String("State: ") + names[si] + (busy || cs.framesWritten > 0 ? ".  Frames written " + juce::String(cs.framesWritten) + ", repeated " + juce::String(cs.framesRepeated) : juce::String()),
                          cs.state == CaptureState::Failed ? Tone::Danger : cs.state == CaptureState::Completed ? Tone::Ok : Tone::Normal);
        const bool repeatedWarning = cs.framesWritten > 0 &&
            static_cast<std::int64_t>(cs.framesRepeated) * 100 > static_cast<std::int64_t>(cs.framesWritten) * 2;
        ch |= qualityWarning_->set(repeatedWarning ? "Quality warning: more than 2% of frames were repeated. Motion may look less smooth." : "",
                                   Tone::Warn);
        // A still-save or rejected-start result belongs to the user action, not to this poll.
        // Keep it until another action or an actual capture state transition supersedes it.
        if (hasActionResult_ && cs.state != actionResultState_) hasActionResult_ = false;
        ch |= msg_->set(hasActionResult_ ? actionResult_ : cs.message,
                        hasActionResult_ ? actionResultTone_ : cs.state == CaptureState::Failed ? Tone::Danger : Tone::Dim);
        progressValue_ = juce::jlimit(0.0, 1.0, cs.progress);
        record_->setEnabled(!busy && dest_file_ != juce::File() && (format_ != CaptureFormat::Mp4 || cs.mp4Available));
        stop_->setEnabled(busy);
        const bool png = format_ == CaptureFormat::Png;
        for (auto* c : std::initializer_list<juce::Component*>{&png_, &gif_, &mp4_, &framing_.box, &dest_ctl()}) c->setEnabled(!busy && (c != &mp4_ || cs.mp4Available));
        duration_.setEnabled(!busy && !png);
        audio_.setEnabled(!busy && format_ == CaptureFormat::Mp4);
        loop_.setEnabled(!busy && format_ == CaptureFormat::Gif);
        ch |= destNote_->set(dest_file_ == juce::File() ? "No destination chosen yet: choose a file before recording." : "Destination: " + dest_file_.getFullPathName(), dest_file_ == juce::File() ? Tone::Warn : Tone::Dim);
        if (ch) relayout();
    }

    void setDestination(const juce::File& destination) {
        if (destination == juce::File()) return;
        clearActionResult();
        dest_file_ = destination.getFileExtension().isEmpty() ? destination.withFileExtension(ext()) : destination;
        refresh(bridge.status());
    }

private:
    juce::Component& dest_ctl() { return *dest_; }
    juce::TextButton& fmtButton(int i) { return i == 0 ? png_ : i == 1 ? gif_ : mp4_; }
    void setFormat(CaptureFormat f) {
        if (format_ != f) clearActionResult();
        format_ = f;
        const double maxS = f == CaptureFormat::Mp4 ? 60.0 : 15.0;
        duration_.slider.setRange(1.0, maxS, 1.0);
        if (duration_.slider.getValue() > maxS) duration_.slider.setValue(maxS, juce::dontSendNotification);
        duration_.slider.updateText();
        durNote_->set(f == CaptureFormat::Png ? "Duration does not apply to a PNG screenshot."
                      : f == CaptureFormat::Gif ? "GIF recordings are limited to 15 seconds."
                                                : "MP4 recordings are limited to 60 seconds.");
        record_->setButtonText(f == CaptureFormat::Png ? "Save PNG" : "Record");
        record_->setTitle(record_->getButtonText());
        if (!dest_file_.isDirectory() && dest_file_ != juce::File() && dest_file_.getFileExtension() != ext()) dest_file_ = dest_file_.withFileExtension(ext());
        relayout();
    }
    juce::String ext() const { return format_ == CaptureFormat::Png ? ".png" : format_ == CaptureFormat::Gif ? ".gif" : ".mp4"; }
    void chooseDestination() {
        chooser_ = std::make_unique<juce::FileChooser>("Capture destination", juce::File::getSpecialLocation(juce::File::userPicturesDirectory).getChildFile("CircuitDrift" + ext()), "*" + ext());
        juce::Component::SafePointer<CapturePanel> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe](const juce::FileChooser& fc) {
                                  if (safe == nullptr) return;
                                  safe->setDestination(fc.getResult());
                              });
    }
    void clearActionResult() { hasActionResult_ = false; }
    void showActionResult(const juce::String& message, Tone tone) {
        actionResult_ = message;
        actionResultTone_ = tone;
        actionResultState_ = bridge.captureStatus().state;
        hasActionResult_ = true;
        msg_->set(message, tone);
    }
    juce::Image snapshot() {
        const auto framing = static_cast<CaptureFraming>(juce::jlimit(0, 2, framing_.index()));
        // Resolve only an explicitly registered editor among this panel's ancestors.
        // A standalone panel has no capture source; arbitrary parents are never a fallback.
        for (auto* ancestor = getParentComponent(); ancestor != nullptr; ancestor = ancestor->getParentComponent())
            if (auto* editor = shell::EditorCore::find(ancestor)) return editor->captureSnapshot(format_, framing);
        return {};
    }
    void startRecording() {
        juce::String err;
        if (dest_file_ == juce::File()) return;
        clearActionResult();
        if (format_ == CaptureFormat::Png) {
            auto img = snapshot();
            if (!img.isValid() || !bridge.savePng(img, dest_file_, err)) showActionResult("PNG failed: " + (err.isEmpty() ? juce::String("could not render the canvas") : err), Tone::Danger);
            else showActionResult("Saved " + dest_file_.getFullPathName(), Tone::Ok);
            relayout();
            return;
        }
        CaptureRequest rq;
        rq.format = format_;
        rq.framing = static_cast<CaptureFraming>(juce::jlimit(0, 2, framing_.index()));
        rq.seconds = duration_.slider.getValue();
        rq.includeAudio = format_ == CaptureFormat::Mp4 && audio_.getToggleState();
        rq.loop = loop_.getToggleState();
        rq.destination = dest_file_;
        if (!bridge.startCapture(rq, err)) { showActionResult("Could not start: " + err, Tone::Danger); relayout(); return; }
        refresh(bridge.status());
    }

    ParamChoice framing_;
    ParamSlider duration_;
    ParamToggle audio_, loop_;
    juce::TextButton png_, gif_, mp4_;
    std::unique_ptr<juce::TextButton> dest_, record_, stop_;
    double progressValue_ = 0.0;
    std::unique_ptr<juce::ProgressBar> progress_;
    NoteLabel *mp4Reason_ = nullptr, *durNote_ = nullptr, *destNote_ = nullptr, *state_ = nullptr, *qualityWarning_ = nullptr, *msg_ = nullptr;
    CaptureFormat format_ = CaptureFormat::Gif;
    juce::File dest_file_;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool hasActionResult_ = false;
    juce::String actionResult_;
    Tone actionResultTone_ = Tone::Normal;
    CaptureState actionResultState_ = CaptureState::Idle;
};

// ------------------------------------------------------------------------------------ Network
class NetworkPanel final : public PanelBase {
public:
    explicit NetworkPanel(EditorBridge& b)
        : PanelBase(b),
          on_(b, "Network enabled", "Join the local multi-instance network (off by default)"),
          bleedOn_(b, "Audio bleed enabled", "Let other instances leak a little of their sound into this one"),
          bleed_(b, "Bleed level", "Level of the cross-talk from other instances") {
        setTitle("Network");
        heading("Multi-instance network");
        on_.bind("network.enabled");
        bleedOn_.bind("network.bleedEnabled");
        bleed_.bind("network.bleedDb");
        content_.addItem(on_, 240, ParamToggle::kHeight, true);
        content_.addItem(bleedOn_, 240, ParamToggle::kHeight, true);
        content_.addItem(bleed_, 240, ParamSlider::kHeight, true);
        status_ = note("", Tone::Normal);
        heading("Limitations");
        lim1_ = note("", Tone::Normal);
        note("Instances may run in different processes or at different times; a host does not guarantee they all run together. Coupling is best effort and uses the last completed result only.");
        note("Offline renders (bounce or export) are not guaranteed to match live playback. Exact offline results need a planned replay or co-render bundle that is not available yet.");
        note("Instances must share one sample rate and one project group; others stay isolated. Everything is local: nothing leaves this computer.");
    }
    void refresh(const StatusInfo& s) override {
        bool ch = false;
        const bool joined = s.networkStatus.isNotEmpty() && !s.networkStatus.equalsIgnoreCase("off");
        ch |= status_->set("Status: " + (s.networkStatus.isEmpty() ? juce::String("Off") : s.networkStatus), joined ? Tone::Ok : Tone::Dim);
        const double ms = s.sampleRate > 0 ? 1024.0 * 1000.0 / s.sampleRate : 0.0;
        ch |= lim1_->set("Live coupling is approximate: shared supply demand and audio bleed arrive about 8 blocks of 128 samples late (1024 samples" +
                             (ms > 0 ? ", about " + juce::String(ms, 1) + " ms at the current rate" : juce::String()) +
                             "). This delay is cross-talk, not reported latency; the main path is never delayed to hide it.",
                         Tone::Normal);
        bleed_.setEnabled(bleedOn_.binding.bound() && bleedOn_.getToggleState());
        if (ch) relayout();
    }

private:
    ParamToggle on_, bleedOn_;
    ParamSlider bleed_;
    NoteLabel *status_ = nullptr, *lim1_ = nullptr;
};

// ---------------------------------------------------------------------------------- Community
class CommunityPanel final : public PanelBase {
public:
    explicit CommunityPanel(EditorBridge& b) : PanelBase(b) {
        setTitle("Community");
        heading("Community presets");
        status_ = note("", Tone::Warn);
        heading("What would be available");
        note("Browse and search shared presets, preview their metadata and licence, and download validated presets into your local library.");
        note("Rate, report and publish your own presets after a moderation step. Publishing never happens without an explicit action.");
        note("This needs an optional catalog service that is not configured yet (gate G06). This panel makes no network calls; your local presets, A/B and everything else work fully offline.");
    }
    void refresh(const StatusInfo& s) override {
        if (status_->set(s.communityStatus.isEmpty() ? juce::String("No catalog service configured (G06)") : s.communityStatus, Tone::Warn)) relayout();
    }

private:
    NoteLabel* status_ = nullptr;
};

}  // namespace

std::unique_ptr<Panel> makeCapturePanel(EditorBridge& b) { return std::make_unique<CapturePanel>(b); }
std::unique_ptr<Panel> makeNetworkPanel(EditorBridge& b) { return std::make_unique<NetworkPanel>(b); }
std::unique_ptr<Panel> makeCommunityPanel(EditorBridge& b) { return std::make_unique<CommunityPanel>(b); }

bool testing::setCaptureDestination(Panel& panel, const juce::File& destination) {
    if (auto* capture = dynamic_cast<CapturePanel*>(&panel)) {
        capture->setDestination(destination);
        return true;
    }
    return false;
}

}  // namespace cd::plugin::ui::pc
