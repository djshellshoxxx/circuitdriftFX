// CircuitDrift plugin UI: Workbench analytical displays (UX-04 thermal scope, UX-05 rail waveform
// and meters, UX-06 phase scatter). Message-thread components fed one telemetry frame per UI frame;
// no timers, no audio coupling. Math lives in AnalysisMath (pure, unit-tested).
#pragma once
#include <deque>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/EngineTypes.h"
#include "ui/Theme.h"
#include "ui/analysis/AnalysisMath.h"

namespace cd::plugin::ui::analysis {

// Per-frame context shared by every display (so they stay aligned to one snapshot).
struct FrameInfo {
    bool valid = false;           // a telemetry frame is available
    bool stale = false;           // >250 ms without a new frame: show badge, fade, do not extrapolate
    int slot = 0, channel = 0;    // edit slot and displayed channel
    int selected = -1;            // selected ComponentId (-1: none)
    double ambientC = 25.0;
    double bpm = 0;
    bool reduceMotion = false;
    bool stereo = true;           // false: mono processing (UX-06 shows Not applicable)
    double nowMs = 0;
};

// Thermal gradient colour for a position from pcb::thermalPosition (0 cold, 0.5 normal, 1 stress).
juce::Colour thermalColour(const Theme& t, double position);
// Signed voltage colour: negative -> voltageNeg, positive -> voltagePos, fading to the panel near 0.
juce::Colour signedColour(const Theme& t, double normalizedSigned);

class DisplayBase : public juce::Component {
public:
    void setTheme(const Theme& t);
    virtual void update(const engine::TelemetryFrame& f, const FrameInfo& info) = 0;
protected:
    const Theme* theme_ = &darkTheme();
    bool stale_ = false;
    virtual void themeChanged() {}
    // Panel chrome: border, title, optional STALE badge. Returns the content rectangle.
    juce::Rectangle<float> paintChrome(juce::Graphics& g, const juce::String& title, const juce::String& right = {}) const;
    void styleButton(juce::TextButton& b, bool toggle) const;
    juce::Font font(float h, bool bold = false) const;
};

// ---- UX-04 -------------------------------------------------------------------------------------------
class TransferScope final : public DisplayBase {
public:
    enum class Mode { TransferXY = 0, VoltTemperature, VoltRail, MagneticBH };
    enum class ColourSource { Temperature, Stress };
    TransferScope();
    void update(const engine::TelemetryFrame& f, const FrameInfo& info) override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void setMode(Mode m);
    Mode mode() const noexcept { return mode_; }
    void setColourSource(ColourSource c);
    // Test/inspection hooks.
    bool bhAvailable() const noexcept { return bhAvailable_; }
    XyMap xyMap() const noexcept { return xyMap_; }
    juce::Rectangle<float> plotRect() const noexcept { return plot_; }
    double smallSignalSlopeValue() const noexcept { return slope_; }
    juce::String footerText() const { return footer_; }
protected:
    void themeChanged() override;
private:
    void layoutPlot();
    juce::Colour traceColour() const;
    struct Trace { std::vector<juce::Point<float>> pts; };
    Mode mode_ = Mode::TransferXY;
    ColourSource colourSource_ = ColourSource::Temperature;
    juce::TextButton tabs_[4], colourButton_;
    juce::Rectangle<float> plot_, content_;
    XyMap xyMap_{};
    std::deque<Trace> history_;                 // persistence: newest last
    std::vector<double> bhH_, bhB_;
    std::deque<std::pair<double, double>> vtPoints_, vrPoints_;  // per-frame (input peak, value)
    bool bhAvailable_ = false;
    double slope_ = std::nan("");
    double tempC_ = 25, tempPos_ = 0, rail_ = 0, railNominal_ = 0, droop_ = 0;
    bool clipping_ = false;
    int selected_ = 2, latency_ = 0;
    std::int64_t lastSample_ = -1;
    juce::String footer_, tempState_;
    std::uint64_t lastGen_ = 0;
};

// ---- UX-05 -------------------------------------------------------------------------------------------
class TimePlot final : public DisplayBase {
public:
    enum class View { Rail = 0, Temperature };
    TimePlot();
    void update(const engine::TelemetryFrame& f, const FrameInfo& info) override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void setView(View v);
    void setWindowSeconds(double s);
    double windowSeconds() const noexcept { return window_; }
    const RailHistory& railHistory() const noexcept { return rail_; }
    RecoveryResult recovery() const { return recovery_; }
    juce::String footerText() const { return footer_; }
protected:
    void themeChanged() override;
private:
    void paintRail(juce::Graphics& g);
    void paintTemperature(juce::Graphics& g);
    juce::Rectangle<float> plot_;
    View view_ = View::Rail;
    double window_ = 2.0;  // 0.1..10 s
    juce::TextButton tabs_[2], zoomOut_, zoomIn_;
    RailHistory rail_;
    SeriesHistory temps_[engine::kComponentCount];
    RecoveryResult recovery_;
    double fs_ = 48000, railNominal_ = 0, railNow_ = 0, nowSeconds_ = 0, ambientC_ = 25;
    std::int64_t lastSample_ = -1;
    int selected_ = 2;
    bool haveFrame_ = false;
    float cursorX_ = -1;
    juce::String footer_;
};

// ---- UX-06 -------------------------------------------------------------------------------------------
class PhaseScatter final : public DisplayBase {
public:
    enum class Source { InOut = 0, LeftRight };
    enum class Plot { Polar = 0, Cartesian };
    PhaseScatter();
    void update(const engine::TelemetryFrame& f, const FrameInfo& info) override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void setSource(Source s);
    void setPlot(Plot p);
    Source source() const noexcept { return source_; }
    // "" when the display is analysable, otherwise the reason it shows instead of a plot
    // ("Not applicable: mono processing", "L/R sample ring not published by telemetry").
    juce::String unavailableReason() const { return unavailable_; }
    juce::String footerText() const { return footer_; }
    const std::vector<PhaseBin>& bins() const noexcept { return bins_; }
protected:
    void themeChanged() override;
private:
    juce::TextButton sourceTabs_[2], plotTabs_[2];
    Source source_ = Source::InOut;
    Plot plot_ = Plot::Polar;
    std::unique_ptr<CrossSpectrumAnalyzer> analyzer_;
    std::vector<PhaseBin> bins_;
    double corr_ = 0, fs_ = 48000;
    std::int64_t lastSample_ = -1;
    juce::String unavailable_, footer_;
    juce::Rectangle<float> plotArea_;
};

// ---- Level meters ------------------------------------------------------------------------------------
class LevelMeters final : public DisplayBase {
public:
    LevelMeters();
    void update(const engine::TelemetryFrame& f, const FrameInfo& info) override;
    void paint(juce::Graphics& g) override;
    double heldPeak(bool output, int ch) const { return hold_[output ? 1 : 0][ch].held; }
    bool clipLit(bool output, int ch) const { return hold_[output ? 1 : 0][ch].clipLit(); }
protected:
    void themeChanged() override {}
private:
    PeakHold hold_[2][engine::kMaxChannels];
    double peak_[2][engine::kMaxChannels] = {}, rms_[2][engine::kMaxChannels] = {}, gainMatch_ = 1;
    std::int64_t lastSample_ = -1;
    double fs_ = 48000;
    bool stereo_ = true;
};

}  // namespace cd::plugin::ui::analysis
