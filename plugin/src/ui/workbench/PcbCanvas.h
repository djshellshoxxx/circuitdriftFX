// CircuitDrift plugin UI: PCB canvas (CD-F17, UX-01). Vector 2D board, optional oblique "3D"
// projection and an accessible 2D schematic fallback of the same component set. Overlays
// (Thermal/Voltage/Signal) and X-Ray change the display only; direct manipulation edits
// parameters through EditorBridge transactions. Message thread only.
#pragma once
#include <array>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ui/EditorBridge.h"
#include "ui/Theme.h"
#include "ui/analysis/AnalysisMath.h"
#include "ui/workbench/GestureMath.h"
#include "ui/workbench/ParamOps.h"
#include "ui/workbench/PartInfo.h"
#include "ui/workbench/PcbModel.h"

namespace cd::plugin::ui {

class PcbCanvas final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    enum class Tool { Auto = 0, Bias, Supply, AgeBrush, Material };
    enum class ViewMode { Board2D = 0, Board3D, Schematic };

    explicit PcbCanvas(EditorBridge& bridge);
    ~PcbCanvas() override;

    // ---- state pushed by WorkbenchView ----
    void applySettings(const UiSettings& s);
    void ingest(const engine::TelemetryFrame& f, bool valid, double nowMs);
    void setTool(Tool t);
    Tool tool() const noexcept { return tool_; }
    void setViewMode(ViewMode m);
    ViewMode viewMode() const noexcept { return mode_; }
    void setChannel(int ch) { channel_ = std::clamp(ch, 0, engine::kMaxChannels - 1); refreshReadouts(); repaint(); }
    int channel() const noexcept { return channel_; }
    void select(int componentId);              // -1 clears
    int selected() const noexcept { return selected_; }
    std::function<void(int)> onSelect;         // selection changed
    std::function<void()> onStateChanged;      // tool / mode changed from inside (keyboard)

    // ---- read-only state for the inspector / tests ----
    const PartReadout& readout(int id) const { return readouts_[static_cast<std::size_t>(id)]; }
    const engine::TelemetryFrame& frame() const noexcept { return frame_; }
    bool frameValid() const noexcept { return valid_; }
    bool stale() const noexcept { return stale_; }
    int editSlot() const noexcept { return slot_; }
    const engine::EffectiveTargets& effective() const noexcept { return eff_; }
    int overlay() const noexcept { return overlay_; }
    bool xray() const noexcept { return xray_; }
    void setStaleOverrideForTest(bool v) { staleOverride_ = v ? 1 : 0; }
    void clearStaleOverride() { staleOverride_ = -1; }

    // ---- interaction entry points (mouse handlers call these; tests drive them directly) ----
    int hitTest(juce::Point<float> p) const;                  // nearest-depth candidate, no side effects
    bool pointerDown(juce::Point<float> p, juce::ModifierKeys mods);
    void pointerDrag(juce::Point<float> p, juce::ModifierKeys mods);
    void pointerUp(juce::Point<float> p);
    void cancelGesture();                                     // Escape: roll back to pre-gesture values
    bool gestureActive() const noexcept { return drag_ != Drag::None || burstActive_; }
    // Supply tool on the selected capacitor bank / transformer: wheel notch or pinch ratio. A burst
    // (one undo record) closes after 350 ms of inactivity or on endBurst().
    void supplyBurst(double ratio);
    void endBurst();
    juce::Point<float> centreOf(int id) const;                // projected centre (top face)
    int cyclePick(juce::Point<float> p);                      // click picking incl. stacked-object cycling
    // Keyboard alternatives (every audible action reachable without pointing).
    void adjustSelected(int direction, bool fine);
    void resetSelected();
    void setMaterial(int index);                              // explicit enum choice, applied once
    void cycleSelection(int delta);
    void refreshForEdit() { refreshReadouts(); repaint(); }
    juce::String gestureReadout() const { return readout_; }
    std::vector<int> brushSet() const { return brushSet_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseMagnify(const juce::MouseEvent& e, float scaleFactor) override;
    bool keyPressed(const juce::KeyPress& k) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    void visibilityChanged() override { if (!isVisible()) { cancelOrCommitOnLoss(); } }

private:
    static constexpr int kLegendH = 30;
    enum class Drag { None, Orbit, Pan, Bias, Supply, Age };

    void timerCallback() override { endBurst(); }
    void cancelOrCommitOnLoss();
    void refreshReadouts();
    void updateProjection();
    double heightScale() const noexcept { return mode_ == ViewMode::Board3D ? 1.0 : 0.0; }
    bool isSupplyTarget(int id) const noexcept;
    bool isResistor(int id) const noexcept;
    void publishDragValue(bool force);
    void updateHover(juce::Point<float> p);
    juce::String hintFor(int id) const;
    void showMaterialMenu(int id);

    // painting
    void drawBoard(juce::Graphics& g);
    void drawSchematic(juce::Graphics& g);
    void drawTraces(juce::Graphics& g, bool schematic);
    void drawPart(juce::Graphics& g, const pcb::Part& part);
    void drawTopArt(juce::Graphics& g, const pcb::Part& part, juce::Colour body, float alpha);
    void drawSymbol(juce::Graphics& g, const pcb::Part& part);
    void drawFlows(juce::Graphics& g);
    void drawLabels(juce::Graphics& g);
    void drawSelection(juce::Graphics& g);
    void drawLegend(juce::Graphics& g);
    juce::AffineTransform planeTransform(double z) const;
    juce::Point<float> proj(double x, double y, double z) const;
    juce::Colour bodyColour(const pcb::Part& part) const;
    juce::Colour tintFor(const pcb::Part& part, bool& hatch) const;

    EditorBridge& bridge_;
    ParamOps ops_;
    const Theme* theme_ = &darkTheme();
    UiSettings settings_;
    int overlay_ = 1;
    bool xray_ = false, reduceMotion_ = false;
    Tool tool_ = Tool::Auto;
    ViewMode mode_ = ViewMode::Board2D;
    int channel_ = 0, slot_ = 0;
    int selected_ = -1, hovered_ = -1;
    engine::TelemetryFrame frame_{};
    bool valid_ = false, stale_ = false;
    int staleOverride_ = -1;
    engine::EffectiveTargets eff_{};
    std::array<PartReadout, pcb::kPartCount> readouts_{};
    analysis::StaleTracker staleTracker_;
    double nowMs_ = 0, runMaxCurrent_ = 1e-4;
    int frameCounter_ = 0;
    pcb::Projection proj_;

    // gesture state
    Drag drag_ = Drag::None;
    juce::Point<float> downPos_, lastPos_, lastClickPos_;
    double startNorm_ = 0, startC_ = 0, pathSigned_ = 0, lastPublishMs_ = 0, pendingNorm_ = 0;
    juce::String pendingParam_;
    std::vector<int> brushSet_;
    juce::String readout_;
    bool moved_ = false;
    // supply burst (wheel / pinch)
    bool burstActive_ = false;
    double burstStartC_ = 0, burstRatio_ = 1;
    int materialPending_ = -1;
};

}  // namespace cd::plugin::ui
