// Real editor capture surfaces and GIF worker, with a synthetic monotonic clock.
// The only panel seam replaces the native destination chooser; clicks, rendering,
// drawer navigation, frame requests and encoding use the production implementations.
#pragma once
#include "FakeBridge.h"
#include "ui/GifCaptureSession.h"
#include "ui/PluginEditor.h"
#include "ui/panels/CapturePanelTestHooks.h"
#include "ui/panels/PanelCommon.h"
#include "ui/shell/EditorCore.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace cd::plugin::test::capture_ui_regressions {

namespace sh = ui::shell;
namespace pc = ui::pc;

template <class T>
T* control(juce::Component& root, const juce::String& title) {
    for (auto* child : root.getChildren()) {
        if (child->getTitle() == title)
            if (auto* match = dynamic_cast<T*>(child)) return match;
        if (auto* match = control<T>(*child, title)) return match;
    }
    return nullptr;
}

template <class Predicate>
bool waitUntil(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

inline bool terminal(CaptureState state) {
    return state == CaptureState::Completed || state == CaptureState::Cancelled || state == CaptureState::Failed;
}

class CaptureBridge final : public FakeBridge {
public:
    bool readTelemetry(engine::TelemetryFrame& frame) override {
        if (telemetryFailuresRemaining > 0) { --telemetryFailuresRemaining; throw std::bad_alloc(); }
        return FakeBridge::readTelemetry(frame);
    }
    bool startCapture(const CaptureRequest& request, juce::String& error) override {
        if (rejectStart) { error = "Synthetic destination rejection"; return false; }
        indices.clear();
        session = std::make_unique<GifCaptureSession>(request, ++epoch, [this] {
            return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(clockMs.load());
        });
        if (!session->ready()) { error = juce::String(session->startError()); return false; }
        error.clear();
        return true;
    }
    std::optional<CaptureFrameRequest> takeCaptureFrameRequest() override {
        ++requestPolls;
        return session ? session->takeFrameRequest() : std::nullopt;
    }
    void submitCaptureFrame(const CaptureFrameRequest& request, const juce::Image& image) override {
        indices.push_back(request.index);
        lastFrame = image;
        if (session) session->submit(request, image);
    }
    void submitCaptureFrame(const juce::Image& image) override {
        ++legacySubmits;
        if (session) session->submit(image);
    }
    void stopCapture() override {
        ++stopCalls;
        if (session) session->stop();
        if (throwAfterStop) throw std::bad_alloc();
    }
    CaptureStatus captureStatus() override {
        if (statusFailuresRemaining > 0) { --statusFailuresRemaining; throw std::bad_alloc(); }
        auto result = session ? session->status() : idleStatus;
        result.mp4Available = false;
        result.mp4Reason = "Native MP4 encoder unavailable (G07)";
        return result;
    }
    bool savePng(const juce::Image& image, const juce::File&, juce::String& error) override {
        ++pngCalls;
        lastPng = image;
        error = savePngOk ? juce::String() : juce::String("Synthetic PNG write rejection");
        return savePngOk;
    }
    void setClock(std::int64_t milliseconds) {
        clockMs.store(milliseconds);
        if (session) session->notifyClockAdvanced();
    }

    std::atomic<std::int64_t> clockMs{0};
    std::uint64_t epoch = 0;
    bool rejectStart = false, savePngOk = true;
    bool throwAfterStop = false;
    int telemetryFailuresRemaining = 0, statusFailuresRemaining = 0;
    int requestPolls = 0, legacySubmits = 0, stopCalls = 0, pngCalls = 0;
    CaptureStatus idleStatus;
    std::vector<std::uint32_t> indices;
    juce::Image lastFrame, lastPng;
    // Destroy/join before the clock captured by the worker is destroyed.
    std::unique_ptr<GifCaptureSession> session;
};

class ForeignSurface final : public juce::Component {
public:
    void paint(juce::Graphics& graphics) override { ++paints; graphics.fillAll(juce::Colours::magenta); }
    int paints = 0;
};

inline bool samePixels(const juce::Image& a, const juce::Image& b) {
    if (!a.isValid() || !b.isValid() || a.getBounds() != b.getBounds() || a.getFormat() != b.getFormat()) return false;
    const juce::Image::BitmapData left(a, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData right(b, juce::Image::BitmapData::readOnly);
    if (left.pixelStride != right.pixelStride) return false;
    const auto rowBytes = static_cast<std::size_t>(a.getWidth()) * static_cast<std::size_t>(left.pixelStride);
    for (int y = 0; y < a.getHeight(); ++y)
        if (std::memcmp(left.getLinePointer(y), right.getLinePointer(y), rowBytes) != 0) return false;
    return true;
}

inline bool labelHasTone(pc::NoteLabel& label, pc::Tone tone) {
    auto* parent = label.getParentComponent();
    if (!parent || label.getWidth() <= 0 || label.getHeight() <= 0 || label.text().isEmpty()) return false;
    pc::NoteLabel expected(tone);
    expected.set(label.text(), tone);
    expected.setBounds(label.getBounds());
    parent->addChildComponent(expected); // Share the same theme without changing the tested label.
    const auto paint = [](pc::NoteLabel& note) {
        juce::Image image(juce::Image::ARGB, note.getWidth(), note.getHeight(), true, juce::SoftwareImageType());
        juce::Graphics graphics(image);
        note.paint(graphics);
        return image;
    };
    return samePixels(paint(label), paint(expected));
}

inline bool softwareImage(const juce::Image& image) {
    return image.isValid() && image.getPixelData()->createType()->getTypeID() == juce::SoftwareImageType().getTypeID();
}

inline bool hasVisualDetail(const juce::Image& image) {
    if (!image.isValid()) return false;
    const auto first = image.getPixelAt(0, 0);
    int different = 0;
    for (int y = 7; y < image.getHeight(); y += 19)
        for (int x = 7; x < image.getWidth(); x += 19)
            if (image.getPixelAt(x, y) != first && ++different > 30) return true;
    return false;
}

struct GifTiming { bool complete = false, fixedDelay = true; int frames = 0, centiseconds = 0; };

// Walk real GIF block boundaries, rather than searching compressed bytes for marker values.
// Decoder correctness is covered by the media/backend suites; here the playback duration is
// the observable consequence of low-rate or missing editor opportunities.
inline GifTiming timing(const juce::File& file) {
    GifTiming result;
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() < 13) return result;
    const auto* data = static_cast<const unsigned char*>(bytes.getData());
    const auto size = bytes.getSize();
    if (std::memcmp(data, "GIF89a", 6) != 0) return result;
    std::size_t pos = 13;
    if ((data[10] & 0x80) != 0) pos += 3u * (1u << ((data[10] & 7u) + 1u));
    const auto skipBlocks = [&] {
        while (pos < size) {
            const auto length = static_cast<std::size_t>(data[pos++]);
            if (length == 0) return true;
            if (length > size - pos) return false;
            pos += length;
        }
        return false;
    };
    int delay = 0;
    while (pos < size) {
        const auto marker = data[pos++];
        if (marker == 0x3b) { result.complete = pos == size && result.frames > 0; return result; }
        if (marker == 0x21) {
            if (pos >= size) return result;
            const auto label = data[pos++];
            if (label == 0xf9) {
                if (size - pos < 6 || data[pos] != 4 || data[pos + 5] != 0) return result;
                delay = data[pos + 2] | (static_cast<int>(data[pos + 3]) << 8);
                pos += 6;
            } else if (!skipBlocks()) return result;
        } else if (marker == 0x2c) {
            if (size - pos < 9) return result;
            const auto packed = data[pos + 8];
            pos += 9;
            if ((packed & 0x80) != 0) pos += 3u * (1u << ((packed & 7u) + 1u));
            if (pos >= size) return result;
            ++pos; // LZW minimum code size
            if (!skipBlocks()) return result;
            ++result.frames;
            result.centiseconds += delay;
            result.fixedDelay = result.fixedDelay && delay == 5;
        } else return result;
    }
    return result;
}

inline bool chooseFormat(ui::Panel& panel, const char* title) {
    if (auto* button = control<juce::Button>(panel, title)) {
        button->setToggleState(true, juce::dontSendNotification);
        button->onClick();
        return true;
    }
    return false;
}

inline bool startFromPanel(ui::Panel& panel, CaptureBridge& bridge, const juce::File& destination,
                           CaptureFraming framing) {
    auto* choice = control<juce::ComboBox>(panel, "Framing");
    auto* duration = control<juce::Slider>(panel, "Duration");
    auto* record = control<juce::Button>(panel, "Record");
    if (!(choice && duration && record && pc::testing::setCaptureDestination(panel, destination))) return false;
    choice->setSelectedId(static_cast<int>(framing) + 1, juce::sendNotificationSync);
    duration->setValue(1.0, juce::sendNotificationSync);
    record->onClick();
    return bridge.captureStatus().state == CaptureState::Recording;
}

inline int run(const juce::File& outputDirectory) {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::fprintf(stderr, "FAIL capture UI: %s\n", message); }
    };
    const auto directory = outputDirectory.getChildFile("capture_ui");
    if (!directory.createDirectory().wasOk()) { check(false, "capture fixture directory can be created"); return failures; }

    {
        auto bridge = std::make_unique<CaptureBridge>();
        juce::Component host;
        host.setSize(750, 540);
        juce::ComponentBoundsConstrainer constrainer;
        bridge->telemetryFailuresRemaining = 1;
        bool initialReadThrew = false;
        try {
            sh::EditorCore failedCore(host, *bridge, constrainer);
        } catch (const std::bad_alloc&) {
            initialReadThrew = bridge->telemetryFailuresRemaining == 0;
        }
        check(initialReadThrew, "editor construction propagates an initial telemetry failure");
        const bool registryClear = sh::EditorCore::find(&host) == nullptr;
        check(registryClear, "failed editor construction leaves its persistent host unregistered");
        // Do not resolve or dereference a registry entry if failed construction left one behind.
        if (registryClear) {
            sh::EditorCore healthyCore(host, *bridge, constrainer);
            const bool registered = sh::EditorCore::find(&host) == &healthyCore;
            check(registered, "healthy construction on the same host registers the new editor core");
            if (registered) {
                healthyCore.selectPanel(ui::PanelId::Capture);
                auto* panel = control<ui::Panel>(host, "Capture");
                const bool configured = panel && chooseFormat(*panel, "PNG screenshot") &&
                    pc::testing::setCaptureDestination(*panel, directory.getChildFile("retried_editor.png"));
                auto* save = configured ? control<juce::Button>(*panel, "Save PNG") : nullptr;
                check(save != nullptr, "retried editor exposes its PNG save action");
                if (save) save->onClick();
                check(bridge->pngCalls == 1 && bridge->lastPng.getWidth() == 1080 && bridge->lastPng.getHeight() == 1920 &&
                          samePixels(bridge->lastPng, healthyCore.captureSnapshot(CaptureFormat::Png, CaptureFraming::Portrait9x16)),
                      "PNG save resolves the healthy editor after construction is retried");
            }
        }
        check(sh::EditorCore::find(&host) == nullptr, "normal destruction unregisters the persistent host");
    }

    {
        auto bridge = std::make_unique<CaptureBridge>();
        ForeignSurface embeddingHost, unrelatedSibling;
        embeddingHost.setSize(1800, 1100);
        ui::CircuitDriftEditor editor(bridge->processor(), *bridge);
        embeddingHost.addAndMakeVisible(editor);
        editor.setBounds(63, 41, 750, 540);
        embeddingHost.addAndMakeVisible(unrelatedSibling);
        unrelatedSibling.setBounds(0, 0, 1800, 1100); // would cover the view in a host-level snapshot
        auto* core = sh::EditorCore::find(&editor);
        check(core && core->isNarrow(), "embedded real editor uses the narrow drawer layout");
        if (!core) return failures;
        core->setReduceMotion(true);
        const std::array<CaptureFraming, 3> framings{{CaptureFraming::Portrait9x16, CaptureFraming::Square1x1,
                                                    CaptureFraming::Landscape16x9}};
        const std::array<juce::Point<int>, 3> expectedGif{{{540, 960}, {540, 540}, {960, 540}}};
        const std::array<juce::Point<int>, 3> expectedPng{{{1080, 1920}, {1080, 1080}, {1920, 1080}}};
        juce::Image easyReference;
        for (const auto view : {EditorView::Easy, EditorView::Workbench}) {
            core->setView(view);
            core->setDrawerOpen(false);
            core->pumpFrame(0.0, 1000);
            for (std::size_t i = 0; i < framings.size(); ++i) {
                core->setDrawerOpen(false);
                const auto visible = core->captureSnapshot(CaptureFormat::Gif, framings[i]);
                check(visible.getWidth() == expectedGif[i].x && visible.getHeight() == expectedGif[i].y,
                      "GIF snapshot has the declared exact dimensions for every framing");
                check(softwareImage(visible) && hasVisualDetail(visible), "selected view paints detailed pixels directly into software memory");
                core->selectPanel(ui::PanelId::Capture);
                check(samePixels(visible, core->captureSnapshot(CaptureFormat::Gif, framings[i])),
                      "narrow Capture drawer cannot replace the selected visual with its form");
                core->selectPanel(ui::PanelId::Quality);
                check(samePixels(visible, core->captureSnapshot(CaptureFormat::Gif, framings[i])),
                      "switching drawer tabs preserves the owned capture surface");
                const auto still = core->captureSnapshot(CaptureFormat::Png, framings[i]);
                check(still.getWidth() == expectedPng[i].x && still.getHeight() == expectedPng[i].y && softwareImage(still),
                      "PNG uses the shared exact dimensions and software destination");
                if (i == 1) {
                    if (view == EditorView::Easy) easyReference = visible;
                    else check(!samePixels(easyReference, visible), "Workbench selection paints Workbench instead of a cached Easy image");
                }
            }
        }
        check(embeddingHost.paints == 0 && unrelatedSibling.paints == 0,
              "snapshot never paints its embedding host or unrelated sibling");
        check(!core->captureSnapshot(CaptureFormat::Mp4, CaptureFraming::Square1x1).isValid(),
              "unsupported MP4 does not silently render another format");
        check(!core->captureSnapshot(CaptureFormat::Gif, static_cast<CaptureFraming>(99)).isValid(),
              "invalid framing does not allocate a guessed output size");

        core->selectPanel(ui::PanelId::Capture);
        const auto beforeCoveredUpdate = core->captureSnapshot(CaptureFormat::Gif, CaptureFraming::Landscape16x9);
        bridge->advance(3.0);
        core->pumpFrame(1.0 / 30.0, 1017);
        const auto coveredUpdate = core->captureSnapshot(CaptureFormat::Gif, CaptureFraming::Landscape16x9);
        check(!samePixels(beforeCoveredUpdate, coveredUpdate), "selected Workbench receives fresh telemetry while the drawer covers it");
        core->setDrawerOpen(false);
        check(samePixels(coveredUpdate, core->captureSnapshot(CaptureFormat::Gif, CaptureFraming::Landscape16x9)),
              "revealing the covered Workbench does not replace a stale capture image");
        core->selectPanel(ui::PanelId::Capture);
        auto* panel = control<ui::Panel>(editor, "Capture");
        check(panel != nullptr, "production Capture panel is reachable inside the editor");
        if (!panel) return failures;
        auto* mp4 = control<juce::Button>(*panel, "MP4 video");
        check(mp4 && !mp4->isEnabled(), "unavailable MP4 remains explicitly disabled");
        check(chooseFormat(*panel, "PNG screenshot") && pc::testing::setCaptureDestination(*panel, directory.getChildFile("owned.png")),
              "PNG action uses production controls and destination handling");
        auto* record = control<juce::Button>(*panel, "Save PNG");
        auto* message = control<pc::NoteLabel>(*panel, "Capture result");
        check(record && message, "capture action and result controls have stable names");
        if (!(record && message)) return failures;
        record->onClick();
        check(bridge->pngCalls == 1 && samePixels(bridge->lastPng, core->captureSnapshot(CaptureFormat::Png, CaptureFraming::Portrait9x16)),
              "PNG panel action resolves the exact owning editor surface");
        const auto savedMessage = message->text();
        for (int i = 0; i < 3; ++i) panel->refresh(bridge->status());
        check(savedMessage.startsWith("Saved ") && message->text() == savedMessage, "PNG success survives repeated status polls");
        bridge->savePngOk = false;
        record->onClick();
        const auto failedMessage = message->text();
        core->selectPanel(ui::PanelId::Quality);
        core->selectPanel(ui::PanelId::Capture);
        panel->refresh(bridge->status());
        check(failedMessage.contains("Synthetic PNG write rejection") && message->text() == failedMessage,
              "PNG failure survives refresh and drawer navigation");
        pc::testing::setCaptureDestination(*panel, directory.getChildFile("another.png"));
        check(message->text() != failedMessage, "a new destination action clears the previous action result");
        check(chooseFormat(*panel, "GIF animation"), "GIF format control remains available");
        bridge->rejectStart = true;
        record->onClick();
        const auto rejectedMessage = message->text();
        for (int i = 0; i < 3; ++i) panel->refresh(bridge->status());
        check(rejectedMessage.contains("Synthetic destination rejection") && message->text() == rejectedMessage,
              "rejected start result survives unchanged backend status");
        bridge->idleStatus.state = CaptureState::Failed;
        bridge->idleStatus.message = "A later capture state superseded the action";
        panel->refresh(bridge->status());
        check(message->text() == bridge->idleStatus.message, "a capture state transition replaces the old action result");
        check(embeddingHost.paints == 0 && unrelatedSibling.paints == 0, "PNG ancestor resolution also excludes host and sibling painting");

        auto* qualityWarning = control<pc::NoteLabel>(*panel, "Capture quality warning");
        auto* captureState = control<pc::NoteLabel>(*panel, "Capture state");
        auto* captureDestination = control<pc::NoteLabel>(*panel, "Capture destination");
        check(qualityWarning && captureState && captureDestination, "capture quality, state and destination have stable names");
        if (!(qualityWarning && captureState && captureDestination)) return failures;
        const auto destinationText = captureDestination->text();
        bridge->idleStatus.state = CaptureState::Completed;
        bridge->idleStatus.framesWritten = 100;
        bridge->idleStatus.framesRepeated = 2;
        bridge->idleStatus.message = "Saved " + directory.getChildFile("quality.gif").getFullPathName();
        panel->refresh(bridge->status());
        check(qualityWarning->text().isEmpty() && !qualityWarning->isVisible() &&
                  message->text() == bridge->idleStatus.message,
              "exactly two percent repeated frames keeps completion without a quality warning");
        bridge->idleStatus.framesRepeated = 3;
        panel->refresh(bridge->status());
        check(qualityWarning->isVisible() && qualityWarning->text().contains("Quality warning") &&
                  labelHasTone(*qualityWarning, pc::Tone::Warn) && message->text() == bridge->idleStatus.message &&
                  captureDestination->text() == destinationText,
              "above two percent warns explicitly while preserving the saved result and destination");
        record->onClick();
        const auto qualityActionError = message->text();
        for (int i = 0; i < 3; ++i) panel->refresh(bridge->status());
        check(qualityActionError.contains("Synthetic destination rejection") && message->text() == qualityActionError &&
                  qualityWarning->isVisible() && captureDestination->text() == destinationText,
              "quality warning and destination coexist with a persistent local action error");
        bridge->idleStatus.state = CaptureState::Failed;
        bridge->idleStatus.message = "Synthetic encoder failure";
        panel->refresh(bridge->status());
        check(captureState->text().startsWith("State: Failed") && labelHasTone(*captureState, pc::Tone::Danger) &&
                  message->text() == bridge->idleStatus.message && labelHasTone(*message, pc::Tone::Danger) &&
                  qualityWarning->isVisible(),
              "repeated-frame warning preserves Failed state and error severity");
        bridge->idleStatus.state = CaptureState::Completed;
        bridge->idleStatus.framesRepeated = 0;
        panel->refresh(bridge->status());
        check(qualityWarning->text().isEmpty() && !qualityWarning->isVisible(),
              "a completion with no repeated frames clears the quality warning");
        bridge->idleStatus = {};
        panel->refresh(bridge->status());
        check(qualityWarning->text().isEmpty() && !qualityWarning->isVisible(),
              "empty capture status has no stale quality warning");

        // Even a parented standalone panel must fail closed, rather than snapshot that parent.
        ForeignSurface foreignParent;
        auto detached = ui::createPanel(ui::PanelId::Capture, *bridge);
        foreignParent.addAndMakeVisible(*detached);
        detached->setBounds(0, 0, 420, 1400);
        chooseFormat(*detached, "PNG screenshot");
        pc::testing::setCaptureDestination(*detached, directory.getChildFile("detached.png"));
        const int savedBefore = bridge->pngCalls;
        auto* detachedRecord = control<juce::Button>(*detached, "Save PNG");
        check(detachedRecord != nullptr, "standalone Capture panel still exposes its action");
        if (detachedRecord) detachedRecord->onClick();
        auto* detachedMessage = control<pc::NoteLabel>(*detached, "Capture result");
        check(bridge->pngCalls == savedBefore && foreignParent.paints == 0 && detachedMessage &&
                  detachedMessage->text().contains("could not render the canvas"),
              "unregistered parent has no capture authority or top-level fallback");
    }

    for (const bool jittered : {false, true}) {
        auto bridge = std::make_unique<CaptureBridge>();
        ui::CircuitDriftEditor editor(bridge->processor(), *bridge);
        editor.setSize(750, 540);
        auto* core = sh::EditorCore::find(&editor);
        check(core != nullptr, "recording fixture has a real editor core");
        if (!core) continue;
        core->setLowFrameRate(true);
        core->selectPanel(ui::PanelId::Capture);
        auto* panel = control<ui::Panel>(editor, "Capture");
        const auto file = directory.getChildFile(jittered ? "editor_jitter.gif" : "editor_30hz.gif");
        const bool started = panel && startFromPanel(*panel, *bridge, file, CaptureFraming::Square1x1);
        check(started, "real Capture-panel action starts the production GIF session");
        if (!started) continue;
        std::vector<int> opportunities;
        if (jittered) opportunities = {0, 17, 93, 181, 182, 399, 514, 827, 999};
        else for (int i = 0; i <= 30; ++i) opportunities.push_back(i * 33);
        for (std::size_t i = 0; i < opportunities.size(); ++i) {
            if (i == 2) core->selectPanel(ui::PanelId::Quality);
            if (i == 4) core->setDrawerOpen(false);
            if (i == 6) core->setView(EditorView::Workbench);
            bridge->setClock(opportunities[i]);
            bridge->advance(opportunities[i] / 1000.0);
            const auto before = bridge->indices.size();
            core->pumpFrame(1.0 / 30.0, static_cast<juce::uint32>(1000 + opportunities[i]));
            check(bridge->indices.size() - before <= 1, "one UI opportunity renders at most one current frame");
            if (i == 0) check(waitUntil([&] { return bridge->captureStatus().framesWritten > 0; }), "first owned frame reaches the encoder");
        }
        const auto submits = bridge->indices.size();
        bridge->setClock(1000); // deliberately no panel refresh or editor frame at the deadline
        const bool done = waitUntil([&] { return terminal(bridge->captureStatus().state); });
        const auto status = bridge->captureStatus();
        const auto parsed = timing(file);
        check(done && status.state == CaptureState::Completed && status.framesWritten == 20,
              "absolute worker deadline completes twenty ticks despite drawer navigation and UI rate");
        check(parsed.complete && parsed.frames == 20 && parsed.fixedDelay && parsed.centiseconds == 100,
              "GIF block delays preserve one second of playback at low or jittered UI rates");
        check(submits == bridge->indices.size() && bridge->legacySubmits == 0,
              "deadline uses worker repetition rather than borrowed UI or legacy frame callbacks");
        if (jittered) {
            check(bridge->indices == std::vector<std::uint32_t>({0, 1, 3, 7, 10, 16, 19}) && status.framesRepeated >= 13,
                  "jitter requests current absolute indices and repeats missed ticks");
        } else {
            std::vector<std::uint32_t> expected;
            for (std::uint32_t i = 0; i < 20; ++i) expected.push_back(i);
            check(bridge->indices == expected, "30 Hz UI opportunities request all twenty absolute capture ticks");
        }
        check(softwareImage(bridge->lastFrame) && hasVisualDetail(bridge->lastFrame),
              "recorded frames come from the production visual surface");
    }

    {
        auto bridge = std::make_unique<CaptureBridge>();
        auto editor = std::make_unique<ui::CircuitDriftEditor>(bridge->processor(), *bridge);
        editor->setSize(750, 540);
        auto* core = sh::EditorCore::find(editor.get());
        check(core != nullptr, "hidden-editor fixture has an owned core");
        if (!core) return failures;
        core->selectPanel(ui::PanelId::Capture);
        auto* panel = control<ui::Panel>(*editor, "Capture");
        const auto file = directory.getChildFile("editor_hidden.gif");
        const bool started = panel && startFromPanel(*panel, *bridge, file, CaptureFraming::Portrait9x16);
        check(started, "hidden-editor fixture starts capture through the panel");
        if (started) {
            core->pumpFrame(0.0, 1000);
            check(waitUntil([&] { return bridge->captureStatus().framesWritten == 1; }), "hidden fixture admits one visual frame");
            core->setActive(false);
            editor->setVisible(false);
            const int polls = bridge->requestPolls;
            const auto submissions = bridge->indices.size();
            bridge->setClock(1000);
            const bool done = waitUntil([&] { return terminal(bridge->captureStatus().state); });
            const auto status = bridge->captureStatus();
            const auto parsed = timing(file);
            check(done && status.state == CaptureState::Completed && status.framesWritten == 20 && status.framesRepeated == 19,
                  "hidden editor completes through the independent worker and last-frame repetition");
            check(!core->timerRunning() && bridge->requestPolls == polls && bridge->indices.size() == submissions,
                  "hidden completion needs no editor, panel or snapshot callback");
            check(parsed.complete && parsed.frames == 20 && parsed.centiseconds == 100,
                  "hidden interval remains present in the output playback duration");
            panel->refresh(bridge->status());
            auto* warning = control<pc::NoteLabel>(*panel, "Capture quality warning");
            auto* message = control<pc::NoteLabel>(*panel, "Capture result");
            auto* destination = control<pc::NoteLabel>(*panel, "Capture destination");
            check(warning && warning->isVisible() && warning->text().contains("Quality warning") &&
                      message && message->text().startsWith("Saved ") &&
                      destination && destination->text().contains(file.getFullPathName()),
                  "hidden completion warns about nineteen repeated frames without hiding success or destination");
        }
        bridge->setClock(2000);
        core->selectPanel(ui::PanelId::Capture);
        const auto destroyedFile = directory.getChildFile("editor_destroyed.gif");
        const bool restarted = panel && startFromPanel(*panel, *bridge, destroyedFile, CaptureFraming::Landscape16x9);
        check(restarted, "same editor can start a fresh session after hidden completion");
        if (restarted) {
            auto* warning = control<pc::NoteLabel>(*panel, "Capture quality warning");
            check(warning && warning->text().isEmpty() && !warning->isVisible(),
                  "starting a fresh session clears the previous quality warning");
            core->pumpFrame(0.0, 2000);
            check(waitUntil([&] { return bridge->captureStatus().framesWritten == 1; }), "destruction fixture admits its owned frame");
            bridge->setClock(2125);
            const int stops = bridge->stopCalls;
            const auto* oldHost = editor.get();
            bridge->statusFailuresRemaining = 1;
            bridge->throwAfterStop = true;
            editor.reset();
            check(sh::EditorCore::find(oldHost) == nullptr && bridge->stopCalls == stops + 1,
                  "editor destruction contains status/stop allocation failures and attempts finalization exactly once");
            check(waitUntil([&] { return terminal(bridge->captureStatus().state); }) &&
                      bridge->captureStatus().state == CaptureState::Completed && timing(destroyedFile).complete,
                  "worker finishes owned frame data after the editor is destroyed");
        }
    }
    return failures;
}

}  // namespace cd::plugin::test::capture_ui_regressions
