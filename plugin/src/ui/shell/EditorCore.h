// CircuitDrift editor shell: the editor's implementation object (layout, header/status, macros,
// utilities, Easy view, drawer, timer-driven refresh, settings persistence). PluginEditor.cpp is a
// thin JUCE AudioProcessorEditor around it. EditorCore also offers a small introspection surface
// used by the snapshot test (essential controls, badges, pumping a frame without a timer).
// Threading: message thread only. Talks to the processor only through EditorBridge.
#pragma once
#include <memory>
#include <vector>
#include "ui/panels/Panels.h"
#include "ui/shell/HelpOverlay.h"
#include "ui/shell/ToneSphere.h"

namespace cd::plugin::ui::shell {

class EditorCore {
public:
    // `host` is the AudioProcessorEditor; `constrainer` is its resize constrainer (limits follow scale).
    EditorCore(juce::Component& host, EditorBridge& bridge, juce::ComponentBoundsConstrainer& constrainer);
    ~EditorCore();
    EditorCore(const EditorCore&) = delete;
    EditorCore& operator=(const EditorCore&) = delete;

    // Registry so tests can reach the core of an editor they constructed.
    static EditorCore* find(const juce::Component* host);

    void hostResized();                 // host bounds changed: recompute scale and reflow
    void setActive(bool visible);       // starts/stops the refresh timer (stopped when hidden)
    // One UI frame: reads telemetry ONCE and fans it out (sphere, workbench, panels, badges).
    void pumpFrame(double dtSeconds, juce::uint32 nowMs);

    // Paint only this editor's selected Easy/Workbench surface into an exact-size software image.
    // The selected surface can be covered by the drawer. No parent or host content is traversed.
    juce::Image captureSnapshot(CaptureFormat format, CaptureFraming framing);

    // ---- State ----
    ShellContext& context() noexcept;
    HelpOverlay& help() noexcept;
    ToneSphere& sphere() noexcept;
    bool isLite() const noexcept;
    bool isNarrow() const noexcept;                 // logical width < 900
    double effectiveScale() const noexcept;         // pixels per logical unit actually in use
    juce::Rectangle<int> logicalBounds() const noexcept;
    int refreshRateHz() const noexcept;
    bool timerRunning() const noexcept;

    void setView(EditorView view);
    void setDarkTheme(bool dark);
    void setUserScale(float scale);
    void setEducational(bool on);
    void setReduceMotion(bool on);
    void setLowFrameRate(bool on);
    void setDrawerOpen(bool open);
    void selectPanel(PanelId id);
    bool drawerOpen() const noexcept;
    PanelId currentPanel() const noexcept;
    EditorView view() const noexcept;
    void applyScene(int index);                     // quick scenes (one undo transaction)
    static int sceneCount() noexcept;
    static const char* sceneName(int index) noexcept;
    bool sceneMatches(int index) const;
    void showSettingsMenu();

    // ---- Introspection for tests ----
    struct Item { juce::String name; juce::Component* comp = nullptr; };
    std::vector<Item> essentialControls() const;    // visible controls that must never clip or overlap
    std::vector<juce::Component*> allKnobs() const;
    juce::String qualityBadgeText() const;
    juce::StringArray statusBadgeTexts() const;
    bool staleBadgeShown() const noexcept;
    juce::String presetLabelText() const;

private:
    struct Parts;
    std::unique_ptr<Parts> p_;
};

}  // namespace cd::plugin::ui::shell
