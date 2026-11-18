#include "ui/analysis/AnalysisDisplays.h"

#include "ui/workbench/PcbModel.h"

namespace cd::plugin::ui::analysis {
namespace {

using juce::Colour;
using juce::Graphics;
using juce::Rectangle;

double niceStep(double rough) {
    const double p = std::pow(10.0, std::floor(std::log10(std::max(rough, 1e-12))));
    const double m = rough / p;
    return (m < 1.5 ? 1.0 : m < 3.5 ? 2.0 : m < 7.5 ? 5.0 : 10.0) * p;
}

juce::String num(double v, int decimals) { return juce::String(v, decimals); }
juce::String signedNum(double v, int decimals) { return (v >= 0 ? "+" : "") + juce::String(v, decimals); }
int decimalsFor(double step) { return step >= 1 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3; }

void dashedLine(Graphics& g, float x0, float y0, float x1, float y1, float w = 1.0f) {
    juce::Path src, dst;
    src.startNewSubPath(x0, y0);
    src.lineTo(x1, y1);
    const float dashes[2] = {4.0f, 3.0f};
    juce::PathStrokeType(w).createDashedStroke(dst, src, dashes, 2);
    g.fillPath(dst);
}

}  // namespace

Colour thermalColour(const Theme& t, double pos) {
    pos = std::clamp(pos, 0.0, 1.0);
    return pos < 0.5 ? t.thermalCold.interpolatedWith(t.thermalNormal, static_cast<float>(pos / 0.5))
                     : t.thermalNormal.interpolatedWith(t.thermalHot, static_cast<float>((pos - 0.5) / 0.5));
}

Colour signedColour(const Theme& t, double v) {
    v = std::clamp(v, -1.0, 1.0);
    const Colour base = v < 0 ? t.voltageNeg : t.voltagePos;
    return t.panel.interpolatedWith(base, static_cast<float>(0.25 + 0.75 * std::abs(v)));
}

// ---- DisplayBase ----------------------------------------------------------------------------------
void DisplayBase::setTheme(const Theme& t) {
    theme_ = &t;
    themeChanged();
    repaint();
}

juce::Font DisplayBase::font(float h, bool bold) const {
    juce::Font f{juce::FontOptions(h)};
    return bold ? f.boldened() : f;
}

void DisplayBase::styleButton(juce::TextButton& b, bool toggle) const {
    b.setClickingTogglesState(toggle);
    b.setColour(juce::TextButton::buttonColourId, theme_->panelRaised);
    b.setColour(juce::TextButton::buttonOnColourId, theme_->accent);
    b.setColour(juce::TextButton::textColourOffId, theme_->text);
    b.setColour(juce::TextButton::textColourOnId, theme_->textInverse);
}

Rectangle<float> DisplayBase::paintChrome(Graphics& g, const juce::String& title, const juce::String& right) const {
    const auto b = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(theme_->panel);
    g.fillRoundedRectangle(b, theme_->corner);
    g.setColour(theme_->border.withAlpha(0.7f));
    g.drawRoundedRectangle(b, theme_->corner, 1.0f);
    auto top = b.reduced(7.0f, 3.0f).removeFromTop(16.0f);
    g.setColour(theme_->textDim);
    g.setFont(font(11.0f, true));
    g.drawText(title, top, juce::Justification::centredLeft, true);
    if (right.isNotEmpty()) {
        g.setFont(font(10.0f));
        g.drawText(right, top, juce::Justification::centredRight, true);
    }
    if (stale_) {
        const auto badge = Rectangle<float>(b.getRight() - 58.0f, b.getY() + 3.0f, 52.0f, 14.0f);
        g.setColour(theme_->warning);
        g.fillRoundedRectangle(badge, 3.0f);
        g.setColour(theme_->textInverse);
        g.setFont(font(10.0f, true));
        g.drawText("! STALE", badge, juce::Justification::centred);
    }
    return b.reduced(6.0f).withTrimmedTop(16.0f);
}

// ---- TransferScope ----------------------------------------------------------------------------------
TransferScope::TransferScope() {
    static const char* names[4] = {"XY", "V-T", "V-R", "B-H"};
    static const char* tips[4] = {"Transfer XY: circuit input vs output, latency-aligned, equal scales",
                                  "Voltage / Temperature: input peak [FS] vs selected component [deg C]",
                                  "Voltage / Rail: input peak [FS] vs reservoir rail [V]",
                                  "Magnetic B-H: only when the transformer model supplies H and B"};
    for (int i = 0; i < 4; ++i) {
        tabs_[i].setButtonText(names[i]);
        tabs_[i].setTooltip(tips[i]);
        tabs_[i].setRadioGroupId(7101);
        tabs_[i].setTitle(tips[i]);
        tabs_[i].onClick = [this, i] { setMode(static_cast<Mode>(i)); };
        addAndMakeVisible(tabs_[i]);
    }
    colourButton_.setButtonText("T/S");
    colourButton_.setTooltip("Trace colour source: Temperature (model-specific range) or Stress (droop/clip/temperature indicators)");
    colourButton_.setTitle("Trace colour source");
    colourButton_.onClick = [this] { setColourSource(colourSource_ == ColourSource::Temperature ? ColourSource::Stress : ColourSource::Temperature); };
    addAndMakeVisible(colourButton_);
    tabs_[0].setToggleState(true, juce::dontSendNotification);
    themeChanged();
}

void TransferScope::themeChanged() {
    for (auto& t : tabs_) styleButton(t, true);
    styleButton(colourButton_, false);
}

void TransferScope::setMode(Mode m) {
    mode_ = m;
    for (int i = 0; i < 4; ++i) tabs_[i].setToggleState(i == static_cast<int>(m), juce::dontSendNotification);
    resized();
    repaint();
}

void TransferScope::setColourSource(ColourSource c) {
    colourSource_ = c;
    colourButton_.setButtonText(c == ColourSource::Temperature ? "T" : "S");
    repaint();
}

void TransferScope::layoutPlot() {
    content_ = getLocalBounds().toFloat().reduced(6.0f).withTrimmedTop(16.0f);
    auto area = content_;
    area.removeFromTop(21.0f);   // tabs row
    area.removeFromBottom(37.0f);  // footer lines
    area.removeFromLeft(4.0f);
    const float side = std::max(20.0f, std::min(area.getWidth() - 4.0f, area.getHeight()));
    plot_ = Rectangle<float>(area.getCentreX() - side * 0.5f, area.getY(), side, side);
    xyMap_ = makeEqualScaleMap(plot_.getX(), plot_.getY(), plot_.getWidth(), plot_.getHeight(), 1.0);
}

void TransferScope::resized() {
    layoutPlot();
    auto row = content_.withHeight(19.0f).toNearestInt();
    colourButton_.setBounds(row.removeFromRight(28));
    row.removeFromRight(2);
    const int w = row.getWidth() / 4;
    for (int i = 0; i < 4; ++i) tabs_[i].setBounds(row.removeFromLeft(w).reduced(1, 0));
}

void TransferScope::update(const engine::TelemetryFrame& f, const FrameInfo& info) {
    stale_ = info.stale || !info.valid;
    if (!info.valid) { repaint(); return; }
    selected_ = info.selected >= 0 ? info.selected : static_cast<int>(engine::ComponentId::V1);
    const int ch = std::clamp(info.channel, 0, engine::kMaxChannels - 1);
    const auto& st = f.slot[std::clamp(info.slot, 0, engine::kSlots - 1)];
    if (lastSample_ >= 0 && f.sampleIndex < lastSample_) {  // seek / graph change: nothing may cross it
        history_.clear(); vtPoints_.clear(); vrPoints_.clear();
    }
    const bool advanced = f.sampleIndex != lastSample_;
    lastSample_ = f.sampleIndex;
    latency_ = f.latencySamples;
    tempC_ = st.temperatureK[ch][selected_] - 273.15;
    const auto& ref = pcb::parts()[static_cast<std::size_t>(selected_)].ref;
    tempPos_ = pcb::thermalPosition(ref, tempC_);
    tempState_ = pcb::thermalStateName(pcb::thermalState(ref, tempC_));
    rail_ = st.railVolts;
    railNominal_ = st.railNominalVolts;
    droop_ = railNominal_ > 0 ? std::max(0.0, 1.0 - rail_ / railNominal_) : 0.0;
    clipping_ = f.outPeak[ch] >= 1.0;
    if (!advanced) { repaint(); return; }

    double x[engine::kRingSize], y[engine::kRingSize], h[engine::kRingSize], b[engine::kRingSize];
    double col0[engine::kRingSize], col1[engine::kRingSize];
    for (int i = 0; i < engine::kRingSize; ++i) { col0[i] = f.transferRing[i][0]; col1[i] = f.transferRing[i][1]; }
    unrollRing(col0, engine::kRingSize, f.transferWrite, x);
    unrollRing(col1, engine::kRingSize, f.transferWrite, y);
    std::vector<double> ox, oy;
    decimateOrderedExtrema(x, y, engine::kRingSize, 2048, ox, oy);
    Trace tr;
    tr.pts.reserve(ox.size());
    for (std::size_t i = 0; i < ox.size(); ++i) tr.pts.emplace_back(static_cast<float>(ox[i]), static_cast<float>(oy[i]));
    history_.push_back(std::move(tr));
    while (history_.size() > (info.reduceMotion ? 1u : 5u)) history_.pop_front();
    slope_ = smallSignalSlope(x, y, engine::kRingSize, 0.3);

    double hr[engine::kRingSize], br[engine::kRingSize];
    for (int i = 0; i < engine::kRingSize; ++i) { hr[i] = st.bhRing[i][0]; br[i] = st.bhRing[i][1]; }
    unrollRing(hr, engine::kRingSize, f.ringWrite, h);
    unrollRing(br, engine::kRingSize, f.ringWrite, b);
    double hMax = 0, bMax = 0;
    bool finite = true;
    for (int i = 0; i < engine::kRingSize; ++i) {
        finite = finite && std::isfinite(h[i]) && std::isfinite(b[i]);
        hMax = std::max(hMax, std::abs(h[i])); bMax = std::max(bMax, std::abs(b[i]));
    }
    // D10: B-H only when the model really supplies magnetic H and B (non-degenerate, finite rings).
    bhAvailable_ = finite && hMax > 1e-9 && bMax > 1e-9;
    bhH_.assign(h, h + engine::kRingSize);
    bhB_.assign(b, b + engine::kRingSize);

    vtPoints_.emplace_back(f.inPeak[ch], tempC_);
    vrPoints_.emplace_back(f.inPeak[ch], rail_);
    while (vtPoints_.size() > 150) vtPoints_.pop_front();
    while (vrPoints_.size() > 150) vrPoints_.pop_front();
    repaint();
}

Colour TransferScope::traceColour() const {
    if (colourSource_ == ColourSource::Temperature) return thermalColour(*theme_, tempPos_);
    const double pos = std::max({tempPos_, droop_ / 0.2, clipping_ ? 1.0 : 0.0});
    return thermalColour(*theme_, pos);
}

void TransferScope::paint(Graphics& g) {
    const char* titles[4] = {"THERMAL SCOPE  Transfer XY", "THERMAL SCOPE  Voltage / Temperature",
                             "THERMAL SCOPE  Voltage / Rail", "THERMAL SCOPE  Magnetic B-H"};
    paintChrome(g, titles[static_cast<int>(mode_)], {});
    g.setColour(theme_->inset);
    g.fillRect(plot_);
    const float fade = stale_ ? 0.35f : 1.0f;
    auto small = font(10.0f);
    g.setFont(small);

    auto label = [&](const juce::String& s, Rectangle<float> r, juce::Justification j, Colour c) {
        g.setColour(c);
        g.drawText(s, r, j, true);
    };

    if (mode_ == Mode::TransferXY) {
        for (double v : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            const bool axis = v == 0.0;
            g.setColour(theme_->grid.withAlpha(axis ? 1.0f : 0.55f));
            g.drawLine(static_cast<float>(xyMap_.px(v)), plot_.getY(), static_cast<float>(xyMap_.px(v)), plot_.getBottom(), axis ? 1.2f : 0.6f);
            g.drawLine(plot_.getX(), static_cast<float>(xyMap_.py(v)), plot_.getRight(), static_cast<float>(xyMap_.py(v)), axis ? 1.2f : 0.6f);
        }
        double x0, y0, x1, y1;
        referenceLine(xyMap_, 1.0, 1.0, x0, y0, x1, y1);
        g.setColour(theme_->textDim);
        dashedLine(g, static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1), 1.0f);
        g.saveState();
        g.reduceClipRegion(plot_.toNearestInt());
        const Colour tc = traceColour();
        int age = static_cast<int>(history_.size());
        for (const auto& tr : history_) {
            --age;
            if (tr.pts.size() < 2) continue;
            juce::Path p;
            for (std::size_t i = 0; i < tr.pts.size(); ++i) {
                const float px = static_cast<float>(xyMap_.px(tr.pts[i].x)), py = static_cast<float>(xyMap_.py(tr.pts[i].y));
                if (i == 0) p.startNewSubPath(px, py); else p.lineTo(px, py);
            }
            const float a = (age == 0 ? 1.0f : 0.5f / (1.0f + age)) * fade;
            g.setColour(tc.withAlpha(a));
            g.strokePath(p, juce::PathStrokeType(age == 0 ? 1.6f : 1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.restoreState();
        label("+1", Rectangle<float>(plot_.getRight() - 24, xyMap_.cy + 1, 22, 11), juce::Justification::centredRight, theme_->textDim);
        label("in [FS]", Rectangle<float>(plot_.getRight() - 46, plot_.getBottom() - 12, 44, 11), juce::Justification::centredRight, theme_->text);
        label("out [FS]", Rectangle<float>(plot_.getX() + 3, plot_.getY() + 1, 50, 11), juce::Justification::centredLeft, theme_->text);
        label("1:1", Rectangle<float>(plot_.getRight() - 26, plot_.getY() + 2, 24, 11), juce::Justification::centredRight, theme_->textDim);
        footer_ = (std::isfinite(slope_) ? "slope |in|<0.3 FS = " + num(slope_, 2) : juce::String("slope: n/a")) +
                  " | fixed +-1 FS, equal axes";
    } else if (mode_ == Mode::MagneticBH) {
        if (!bhAvailable_ || bhH_.empty()) {
            g.setColour(theme_->textDim);
            g.setFont(font(11.0f));
            g.drawFittedText("Unavailable: the model does not expose magnetic state (H, B). An XY transfer loop is not magnetic hysteresis.",
                             plot_.reduced(8).toNearestInt(), juce::Justification::centred, 5);
            footer_ = "B-H unavailable (D10): no valid H/B telemetry";
        } else {
            double hm = 1e-9, bm = 1e-9;
            for (std::size_t i = 0; i < bhH_.size(); ++i) { hm = std::max(hm, std::abs(bhH_[i])); bm = std::max(bm, std::abs(bhB_[i])); }
            hm = hm * 1.2; bm = std::max(bm * 1.2, 1e-6);
            const auto mx = [&](double hv) { return plot_.getCentreX() + static_cast<float>(hv / hm) * plot_.getWidth() * 0.5f; };
            const auto my = [&](double bv) { return plot_.getCentreY() - static_cast<float>(bv / bm) * plot_.getHeight() * 0.5f; };
            g.setColour(theme_->grid);
            g.drawLine(plot_.getX(), plot_.getCentreY(), plot_.getRight(), plot_.getCentreY(), 1.2f);
            g.drawLine(plot_.getCentreX(), plot_.getY(), plot_.getCentreX(), plot_.getBottom(), 1.2f);
            juce::Path p;
            for (std::size_t i = 0; i < bhH_.size(); ++i) {
                if (i == 0) p.startNewSubPath(mx(bhH_[i]), my(bhB_[i])); else p.lineTo(mx(bhH_[i]), my(bhB_[i]));
            }
            g.setColour(traceColour().withAlpha(fade));
            g.strokePath(p, juce::PathStrokeType(1.6f));
            label("H [A/m] +-" + num(hm, 0), Rectangle<float>(plot_.getX() + 2, plot_.getBottom() - 12, plot_.getWidth() - 4, 11), juce::Justification::centredRight, theme_->text);
            label("B [T] +-" + num(bm, 2), Rectangle<float>(plot_.getX() + 3, plot_.getY() + 1, 90, 11), juce::Justification::centredLeft, theme_->text);
            footer_ = "dynamic B-H of the synthetic transformer model (G01); autoscaled";
        }
    } else {
        const bool vt = mode_ == Mode::VoltTemperature;
        const auto& pts = vt ? vtPoints_ : vrPoints_;
        double ylo = vt ? tempC_ - 5 : rail_ - 10, yhi = vt ? tempC_ + 5 : rail_ + 10;
        if (!vt && railNominal_ > 0) { ylo = std::min(ylo, railNominal_ * 0.85); yhi = std::max(yhi, railNominal_ * 1.02); }
        for (const auto& pr : pts) { ylo = std::min(ylo, pr.second); yhi = std::max(yhi, pr.second); }
        const double padY = std::max((yhi - ylo) * 0.1, 0.5);
        ylo -= padY; yhi += padY;
        const auto mx = [&](double v) { return plot_.getX() + static_cast<float>(std::clamp(v, 0.0, 1.0)) * plot_.getWidth(); };
        const auto my = [&](double v) { return plot_.getBottom() - static_cast<float>((v - ylo) / (yhi - ylo)) * plot_.getHeight(); };
        g.setColour(theme_->grid.withAlpha(0.6f));
        for (double v : {0.0, 0.25, 0.5, 0.75, 1.0}) g.drawLine(mx(v), plot_.getY(), mx(v), plot_.getBottom(), 0.6f);
        const double ys = niceStep((yhi - ylo) / 4.0);
        for (double v = std::ceil(ylo / ys) * ys; v < yhi; v += ys) {
            g.setColour(theme_->grid.withAlpha(0.6f));
            g.drawLine(plot_.getX(), my(v), plot_.getRight(), my(v), 0.6f);
            label(num(v, decimalsFor(ys)), Rectangle<float>(plot_.getX() + 2, my(v) - 11, 40, 10), juce::Justification::centredLeft, theme_->textDim);
        }
        if (!vt && railNominal_ > 0) {
            g.setColour(theme_->textDim);
            dashedLine(g, plot_.getX(), my(railNominal_), plot_.getRight(), my(railNominal_));
            label("V_nom " + num(railNominal_, 0) + " V", Rectangle<float>(plot_.getRight() - 80, my(railNominal_) - 11, 78, 10), juce::Justification::centredRight, theme_->textDim);
        }
        const Colour tc = traceColour();
        int i = 0;
        for (const auto& pr : pts) {
            const float a = (0.2f + 0.8f * static_cast<float>(++i) / static_cast<float>(pts.size())) * fade;
            g.setColour(tc.withAlpha(a));
            g.fillEllipse(mx(pr.first) - 1.8f, my(pr.second) - 1.8f, 3.6f, 3.6f);
        }
        label(vt ? "input peak [FS]" : "input peak [FS]", Rectangle<float>(plot_.getRight() - 90, plot_.getBottom() - 12, 88, 11), juce::Justification::centredRight, theme_->text);
        label(vt ? "T [" + juce::String(juce::CharPointer_UTF8("\xC2\xB0")) + "C]" : "V_res [V]", Rectangle<float>(plot_.getX() + 3, plot_.getY() + 1, 80, 11), juce::Justification::centredLeft, theme_->text);
        footer_ = vt ? "per-frame pairs; separate units, never summed" : "per-frame pairs, rail " + num(rail_, 1) + " V (" + num(railNominal_ > 0 ? rail_ / railNominal_ : 0, 3) + " of nominal)";
    }
    g.setColour(theme_->border.withAlpha(0.8f));
    g.drawRect(plot_, 1.0f);

    // Footer: two compact lines; colour legend carries numbers, never colour alone.
    juce::String colourLine;
    const auto& comp = pcb::parts()[static_cast<std::size_t>(selected_)];
    if (colourSource_ == ColourSource::Temperature)
        colourLine = juce::String("colour: ") + comp.key + " " + num(tempC_, 1) + juce::String(juce::CharPointer_UTF8(" \xC2\xB0")) + "C " + tempState_;
    else
        colourLine = "stress: T " + num(tempPos_, 2) + " | droop " + num(droop_ * 100, 1) + "% | clip " + (clipping_ ? "yes" : "no");
    auto cc = content_;
    auto foot = cc.removeFromBottom(36.0f);
    g.setFont(font(10.0f));
    g.setColour(theme_->textDim);
    g.drawFittedText(footer_ + "\n" + colourLine, foot.toNearestInt(), juce::Justification::topLeft, 3, 0.9f);
}

// ---- TimePlot -------------------------------------------------------------------------------------------
TimePlot::TimePlot() {
    static const char* names[2] = {"Rail", "Temp"};
    for (int i = 0; i < 2; ++i) {
        tabs_[i].setButtonText(names[i]);
        tabs_[i].setRadioGroupId(7102);
        tabs_[i].setTitle(i == 0 ? "Power rail waveform" : "Temperature history");
        tabs_[i].onClick = [this, i] { setView(static_cast<View>(i)); };
        addAndMakeVisible(tabs_[i]);
    }
    zoomOut_.setButtonText("-");
    zoomOut_.setTitle("Zoom out time window");
    zoomOut_.setTooltip("Zoom out (0.1-10 s window)");
    zoomOut_.onClick = [this] { setWindowSeconds(window_ * 1.5); };
    zoomIn_.setButtonText("+");
    zoomIn_.setTitle("Zoom in time window");
    zoomIn_.setTooltip("Zoom in (0.1-10 s window)");
    zoomIn_.onClick = [this] { setWindowSeconds(window_ / 1.5); };
    addAndMakeVisible(zoomOut_);
    addAndMakeVisible(zoomIn_);
    tabs_[0].setToggleState(true, juce::dontSendNotification);
    themeChanged();
}

void TimePlot::themeChanged() {
    for (auto& t : tabs_) styleButton(t, true);
    styleButton(zoomOut_, false);
    styleButton(zoomIn_, false);
}

void TimePlot::setView(View v) {
    view_ = v;
    for (int i = 0; i < 2; ++i) tabs_[i].setToggleState(i == static_cast<int>(v), juce::dontSendNotification);
    repaint();
}

void TimePlot::setWindowSeconds(double s) {
    window_ = std::clamp(s, 0.1, 10.0);
    repaint();
}

void TimePlot::resized() {
    auto content = getLocalBounds().reduced(6).withTrimmedTop(16);
    auto row = content.removeFromTop(19);
    zoomIn_.setBounds(row.removeFromRight(24));
    row.removeFromRight(2);
    zoomOut_.setBounds(row.removeFromRight(24));
    row.removeFromRight(4);
    const int w = std::min(row.getWidth() / 2, 56);
    tabs_[0].setBounds(row.removeFromLeft(w).reduced(1, 0));
    tabs_[1].setBounds(row.removeFromLeft(w).reduced(1, 0));
    content.removeFromTop(2);
    content.removeFromBottom(38);
    plot_ = content.toFloat().withTrimmedLeft(2.0f).withTrimmedBottom(12.0f);
}

void TimePlot::mouseMove(const juce::MouseEvent& e) { cursorX_ = plot_.contains(e.position) ? e.position.x : -1.0f; repaint(); }
void TimePlot::mouseExit(const juce::MouseEvent&) { cursorX_ = -1.0f; repaint(); }
void TimePlot::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    setWindowSeconds(window_ * std::pow(1.25, -w.deltaY * 2.0));
}

void TimePlot::update(const engine::TelemetryFrame& f, const FrameInfo& info) {
    stale_ = info.stale || !info.valid;
    if (!info.valid) { repaint(); return; }
    const int slot = std::clamp(info.slot, 0, engine::kSlots - 1), ch = std::clamp(info.channel, 0, engine::kMaxChannels - 1);
    selected_ = info.selected >= 0 ? info.selected : static_cast<int>(engine::ComponentId::V1);
    ambientC_ = info.ambientC;
    fs_ = f.sampleRate > 0 ? f.sampleRate : 48000.0;
    const auto& st = f.slot[slot];
    railNominal_ = st.railNominalVolts;
    railNow_ = st.railVolts;
    haveFrame_ = true;
    if (f.sampleIndex != lastSample_) {
        if (lastSample_ >= 0 && f.sampleIndex < lastSample_) {
            rail_.clear();
            for (auto& s : temps_) s.clear();
        }
        lastSample_ = f.sampleIndex;
        nowSeconds_ = static_cast<double>(f.sampleIndex) / fs_;
        rail_.ingest(f.sampleIndex, fs_, st.railRing, f.ringWrite, engine::kRingSize, std::max(f.ringDecimation, 1));
        for (int c = 0; c < engine::kComponentCount; ++c) temps_[c].add(nowSeconds_, st.temperatureK[ch][c] - 273.15);
    }
    const auto t1 = rail_.newestBin() + 1;
    recovery_ = rail_.empty() ? RecoveryResult{} : measureRecovery(rail_, t1 - static_cast<std::int64_t>(window_ * 1000.0), t1);
    repaint();
}

void TimePlot::paint(Graphics& g) {
    paintChrome(g, view_ == View::Rail ? "POWER RAIL  V(t) DC" : "TEMPERATURE HISTORY", num(window_, window_ < 1 ? 2 : 1) + " s");
    g.setColour(theme_->inset);
    g.fillRect(plot_);
    if (view_ == View::Rail) paintRail(g); else paintTemperature(g);
    g.setColour(theme_->border.withAlpha(0.8f));
    g.drawRect(plot_, 1.0f);
    auto foot = getLocalBounds().toFloat().reduced(6.0f).removeFromBottom(38.0f);
    g.setFont(font(10.0f));
    g.setColour(theme_->textDim);
    g.drawFittedText(footer_, foot.toNearestInt(), juce::Justification::topLeft, 3, 0.9f);
}

void TimePlot::paintRail(Graphics& g) {
    g.setFont(font(10.0f));
    if (rail_.empty()) {
        g.setColour(theme_->textDim);
        g.drawFittedText("No rail telemetry yet", plot_.toNearestInt(), juce::Justification::centred, 1);
        footer_ = "DC rail V_res (not AC mains)";
        return;
    }
    const auto t1 = rail_.newestBin() + 1;
    const auto t0 = t1 - static_cast<std::int64_t>(std::llround(window_ * 1000.0));
    const int cols = std::max(2, static_cast<int>(plot_.getWidth()));
    std::vector<EnvelopeColumn> env;
    decimateEnvelope(rail_, t0, t1, cols, env);
    double lo = 1e18, hi = -1e18;
    for (const auto& c : env) if (c.valid) { lo = std::min<double>(lo, c.vmin); hi = std::max<double>(hi, c.vmax); }
    if (lo > hi) { lo = railNow_ - 1; hi = railNow_ + 1; }
    if (railNominal_ > 0) { lo = std::min(lo, railNominal_ * 0.97); hi = std::max(hi, railNominal_ * 1.01); }
    const double pad = std::max((hi - lo) * 0.08, 0.5);
    lo -= pad; hi += pad;
    const auto yOf = [&](double v) { return plot_.getBottom() - static_cast<float>((v - lo) / (hi - lo)) * plot_.getHeight(); };
    const auto xOf = [&](int col) { return plot_.getX() + (static_cast<float>(col) + 0.5f) * plot_.getWidth() / static_cast<float>(cols); };

    // Absolute-time ms grid (sample-derived), labelled in seconds.
    const double tEnd = static_cast<double>(t1) / 1000.0, tStart = tEnd - window_;
    const double ts = niceStep(window_ / 5.0);
    for (double t = std::ceil(tStart / ts) * ts; t < tEnd; t += ts) {
        const float x = plot_.getX() + static_cast<float>((t - tStart) / window_) * plot_.getWidth();
        g.setColour(theme_->grid.withAlpha(0.55f));
        g.drawLine(x, plot_.getY(), x, plot_.getBottom(), 0.6f);
        g.setColour(theme_->textDim);
        g.drawText(num(t, decimalsFor(ts)) + " s", Rectangle<float>(x - 22, plot_.getBottom() + 1, 44, 11), juce::Justification::centred);
    }
    const double vs = niceStep((hi - lo) / 4.0);
    for (double v = std::ceil(lo / vs) * vs; v < hi; v += vs) {
        g.setColour(theme_->grid.withAlpha(0.55f));
        g.drawLine(plot_.getX(), yOf(v), plot_.getRight(), yOf(v), 0.6f);
        g.setColour(theme_->textDim);
        g.drawText(num(v, decimalsFor(vs)), Rectangle<float>(plot_.getX() + 2, yOf(v) - 10, 40, 10), juce::Justification::centredLeft);
    }
    if (railNominal_ > 0) {
        g.setColour(theme_->textDim);
        dashedLine(g, plot_.getX(), yOf(railNominal_), plot_.getRight(), yOf(railNominal_));
        g.drawText("V_nom " + num(railNominal_, 0) + " V", Rectangle<float>(plot_.getRight() - 84, yOf(railNominal_) - 11, 82, 10), juce::Justification::centredRight);
    }
    // Envelope + mean, gaps marked (never bridged).
    int first = -1, last = -1;
    for (int c = 0; c < cols; ++c) if (env[static_cast<std::size_t>(c)].valid) { if (first < 0) first = c; last = c; }
    const float fade = stale_ ? 0.4f : 1.0f;
    g.saveState();
    g.reduceClipRegion(plot_.toNearestInt());
    juce::Path mean;
    bool open = false;
    for (int c = 0; c < cols; ++c) {
        const auto& e = env[static_cast<std::size_t>(c)];
        if (!e.valid) { open = false; continue; }
        g.setColour(theme_->voltagePos.withAlpha(0.35f * fade));
        g.drawLine(xOf(c), yOf(e.vmax), xOf(c), yOf(e.vmin) + 0.6f, 1.0f);
        if (!open) { mean.startNewSubPath(xOf(c), yOf(e.vmean)); open = true; } else mean.lineTo(xOf(c), yOf(e.vmean));
    }
    g.setColour(theme_->voltagePos.withAlpha(fade));
    g.strokePath(mean, juce::PathStrokeType(1.6f));
    if (first >= 0) {
        int c = first;
        while (c < last) {
            if (env[static_cast<std::size_t>(c)].valid) { ++c; continue; }
            int c1 = c;
            while (c1 < last && !env[static_cast<std::size_t>(c1)].valid) ++c1;
            const auto r = Rectangle<float>(xOf(c) - 0.5f, plot_.getY(), xOf(c1) - xOf(c) + 1.0f, plot_.getHeight());
            g.setColour(theme_->warning.withAlpha(0.18f));
            g.fillRect(r);
            g.setColour(theme_->warning);
            g.drawRect(r, 1.0f);
            if (r.getWidth() > 22) { g.setFont(font(9.0f, true)); g.drawText("gap", r.withHeight(11.0f), juce::Justification::centred); }
            c = c1;
        }
    }
    // Minimum marker.
    if (recovery_.status != RecoveryResult::Status::NoEvent) {
        const float mx = plot_.getX() + static_cast<float>((static_cast<double>(recovery_.minBin) / 1000.0 - tStart) / window_) * plot_.getWidth();
        g.setColour(theme_->danger);
        g.drawEllipse(mx - 3.5f, yOf(recovery_.minVolts) - 3.5f, 7.0f, 7.0f, 1.5f);
    }
    // Cursor readout names the 1 ms bin and its aggregation interval, not a fictitious sample.
    juce::String cursorText;
    if (cursorX_ >= 0) {
        const auto bin = t0 + static_cast<std::int64_t>(std::floor((cursorX_ - plot_.getX()) / plot_.getWidth() * window_ * 1000.0));
        g.setColour(theme_->text.withAlpha(0.6f));
        g.drawLine(cursorX_, plot_.getY(), cursorX_, plot_.getBottom(), 1.0f);
        RailBin rb;
        cursorText = "t " + num(static_cast<double>(bin) / 1000.0, 3) + " s (1 ms bin) ";
        cursorText += rail_.binAt(bin, rb) ? num(rb.vmin, 1) + "/" + num(rb.vmean, 1) + "/" + num(rb.vmax, 1) + " V min/mean/max" : "no data (gap)";
        g.setColour(theme_->panel.withAlpha(0.85f));
        g.fillRect(Rectangle<float>(plot_.getX() + 2, plot_.getY() + 2, plot_.getWidth() - 4, 12));
        g.setColour(theme_->text);
        g.setFont(font(10.0f));
        g.drawText(cursorText, Rectangle<float>(plot_.getX() + 4, plot_.getY() + 2, plot_.getWidth() - 8, 12), juce::Justification::centredLeft, true);
    }
    g.restoreState();

    juce::String rec;
    switch (recovery_.status) {
        case RecoveryResult::Status::NoEvent: rec = "no droop event in window"; break;
        case RecoveryResult::Status::Recovered: rec = "min " + num(recovery_.minVolts, 1) + " V, 95% recovery " + num(recovery_.recoveryMs, 0) + " ms (measured)"; break;
        case RecoveryResult::Status::NotRecovered: rec = "min " + num(recovery_.minVolts, 1) + " V, not recovered"; break;
        case RecoveryResult::Status::RejectedOverlap: rec = "min " + num(recovery_.minVolts, 1) + " V; overlapping events: recovery not reported"; break;
        case RecoveryResult::Status::RejectedGap: rec = "min " + num(recovery_.minVolts, 1) + " V; data gap: recovery not reported"; break;
    }
    footer_ = "DC rail V_res now " + num(railNow_, 1) + " V (not AC mains). " + rec + ". Sync target: not set.";
}

void TimePlot::paintTemperature(Graphics& g) {
    g.setFont(font(10.0f));
    const auto& comp = pcb::parts()[static_cast<std::size_t>(selected_)];
    const auto& series = temps_[selected_];
    const double tEnd = nowSeconds_, tStart = tEnd - window_;
    double lo = 1e18, hi = -1e18;
    for (int i = 0; i < series.size(); ++i) if (series.t(i) >= tStart && std::isfinite(series.v(i))) { lo = std::min(lo, series.v(i)); hi = std::max(hi, series.v(i)); }
    if (lo > hi) { lo = ambientC_; hi = ambientC_ + 10; }
    lo = std::min(lo, ambientC_); hi = std::max(hi, comp.ref.normalMax);
    if (comp.ref.stress < hi + 30) hi = std::max(hi, comp.ref.stress);
    const double pad = std::max((hi - lo) * 0.08, 1.0);
    lo -= pad; hi += pad;
    const auto yOf = [&](double v) { return plot_.getBottom() - static_cast<float>((v - lo) / (hi - lo)) * plot_.getHeight(); };
    const auto xOf = [&](double t) { return plot_.getX() + static_cast<float>((t - tStart) / window_) * plot_.getWidth(); };
    const double ts = niceStep(window_ / 5.0);
    for (double t = std::ceil(tStart / ts) * ts; t < tEnd; t += ts) {
        g.setColour(theme_->grid.withAlpha(0.55f));
        g.drawLine(xOf(t), plot_.getY(), xOf(t), plot_.getBottom(), 0.6f);
        g.setColour(theme_->textDim);
        g.drawText(num(t, decimalsFor(ts)) + " s", Rectangle<float>(xOf(t) - 22, plot_.getBottom() + 1, 44, 11), juce::Justification::centred);
    }
    const double vs = niceStep((hi - lo) / 4.0);
    for (double v = std::ceil(lo / vs) * vs; v < hi; v += vs) {
        g.setColour(theme_->grid.withAlpha(0.55f));
        g.drawLine(plot_.getX(), yOf(v), plot_.getRight(), yOf(v), 0.6f);
        g.setColour(theme_->textDim);
        g.drawText(num(v, decimalsFor(vs)), Rectangle<float>(plot_.getX() + 2, yOf(v) - 10, 40, 10), juce::Justification::centredLeft);
    }
    const juce::String degC = juce::String(juce::CharPointer_UTF8("\xC2\xB0")) + "C";
    g.setColour(theme_->thermalNormal);
    dashedLine(g, plot_.getX(), yOf(comp.ref.normalMax), plot_.getRight(), yOf(comp.ref.normalMax));
    g.drawText("normal <= " + num(comp.ref.normalMax, 0) + " " + degC, Rectangle<float>(plot_.getRight() - 110, yOf(comp.ref.normalMax) - 11, 108, 10), juce::Justification::centredRight);
    if (comp.ref.stress <= hi) {
        g.setColour(theme_->thermalHot);
        dashedLine(g, plot_.getX(), yOf(comp.ref.stress), plot_.getRight(), yOf(comp.ref.stress));
        g.drawText("stress >= " + num(comp.ref.stress, 0) + " " + degC, Rectangle<float>(plot_.getRight() - 110, yOf(comp.ref.stress) - 11, 108, 10), juce::Justification::centredRight);
    }
    g.saveState();
    g.reduceClipRegion(plot_.toNearestInt());
    juce::Path p;
    bool open = false;
    double prevT = -1e9;
    for (int i = 0; i < series.size(); ++i) {
        const double t = series.t(i), v = series.v(i);
        if (t < tStart - 1.0) continue;
        if (!std::isfinite(v) || t - prevT > 0.25) {
            if (std::isfinite(prevT) && prevT > -1e8 && t - prevT > 0.25) {  // mark the gap
                const auto r = Rectangle<float>(xOf(prevT), plot_.getY(), std::max(1.0f, xOf(t) - xOf(prevT)), plot_.getHeight());
                g.setColour(theme_->warning.withAlpha(0.16f));
                g.fillRect(r);
            }
            open = false;
        }
        prevT = t;
        if (!std::isfinite(v)) continue;
        if (!open) { p.startNewSubPath(xOf(t), yOf(v)); open = true; } else p.lineTo(xOf(t), yOf(v));
    }
    g.setColour(theme_->thermalNormal.withAlpha(stale_ ? 0.4f : 1.0f));
    g.strokePath(p, juce::PathStrokeType(1.6f));
    g.restoreState();
    g.setColour(theme_->text);
    g.drawText(juce::String(comp.key) + " [" + degC + "]", Rectangle<float>(plot_.getX() + 3, plot_.getY() + 1, 120, 11), juce::Justification::centredLeft);
    const double now = series.size() ? series.v(series.size() - 1) : std::nan("");
    footer_ = juce::String(comp.key) + (std::isfinite(now) ? " " + num(now, 1) + " " + degC + " " + juce::String(pcb::thermalStateName(pcb::thermalState(comp.ref, now))) : juce::String(" no data")) +
              "; reference " + comp.refBasis + " (illustrative, synthetic G01).";
}

// ---- PhaseScatter -----------------------------------------------------------------------------------------
PhaseScatter::PhaseScatter() {
    static const char* src[2] = {"In/Out", "L/R"};
    static const char* pl[2] = {"Polar", "Freq"};
    for (int i = 0; i < 2; ++i) {
        sourceTabs_[i].setButtonText(src[i]);
        sourceTabs_[i].setRadioGroupId(7103);
        sourceTabs_[i].setTitle(i == 0 ? "Phase source: dry input versus wet output" : "Phase source: left versus right channel");
        sourceTabs_[i].onClick = [this, i] { setSource(static_cast<Source>(i)); };
        plotTabs_[i].setButtonText(pl[i]);
        plotTabs_[i].setRadioGroupId(7104);
        plotTabs_[i].setTitle(i == 0 ? "Polar phase plot" : "Frequency versus phase plot");
        plotTabs_[i].onClick = [this, i] { setPlot(static_cast<Plot>(i)); };
        addAndMakeVisible(sourceTabs_[i]);
        addAndMakeVisible(plotTabs_[i]);
    }
    sourceTabs_[0].setToggleState(true, juce::dontSendNotification);
    plotTabs_[0].setToggleState(true, juce::dontSendNotification);
    analyzer_ = std::make_unique<CrossSpectrumAnalyzer>(256, 256, 48000.0);
    themeChanged();
}

void PhaseScatter::themeChanged() {
    for (auto& b : sourceTabs_) styleButton(b, true);
    for (auto& b : plotTabs_) styleButton(b, true);
}

void PhaseScatter::setSource(Source s) {
    source_ = s;
    for (int i = 0; i < 2; ++i) sourceTabs_[i].setToggleState(i == static_cast<int>(s), juce::dontSendNotification);
    repaint();
}

void PhaseScatter::setPlot(Plot p) {
    plot_ = p;
    for (int i = 0; i < 2; ++i) plotTabs_[i].setToggleState(i == static_cast<int>(p), juce::dontSendNotification);
    repaint();
}

void PhaseScatter::resized() {
    auto content = getLocalBounds().reduced(6).withTrimmedTop(16);
    auto row = content.removeFromTop(19);
    const int w = row.getWidth() / 4;
    sourceTabs_[0].setBounds(row.removeFromLeft(w).reduced(1, 0));
    sourceTabs_[1].setBounds(row.removeFromLeft(w).reduced(1, 0));
    plotTabs_[0].setBounds(row.removeFromLeft(w).reduced(1, 0));
    plotTabs_[1].setBounds(row.removeFromLeft(w).reduced(1, 0));
    content.removeFromTop(2);
    content.removeFromBottom(38);
    const float side = std::max(20, std::min(content.getWidth(), content.getHeight()));
    plotArea_ = Rectangle<float>(content.toFloat().getCentreX() - side * 0.5f, content.toFloat().getY(), side, side);
}

void PhaseScatter::update(const engine::TelemetryFrame& f, const FrameInfo& info) {
    stale_ = info.stale || !info.valid;
    if (!info.valid) { repaint(); return; }
    unavailable_.clear();
    if (source_ == Source::LeftRight) {
        // The telemetry frame carries no stereo sample ring (only channel-0 dry/wet), so true L/R
        // phase cannot be computed on the UI side; say so instead of faking it.
        unavailable_ = info.stereo ? "L/R sample ring not published by telemetry (contract request)" : "Not applicable: mono processing";
        bins_.clear();
        repaint();
        return;
    }
    const double fs = f.sampleRate > 0 ? f.sampleRate : 48000.0;
    if (fs != fs_ || !analyzer_) {
        fs_ = fs;
        analyzer_ = std::make_unique<CrossSpectrumAnalyzer>(256, 256, fs);
        lastSample_ = -1;
    }
    if (lastSample_ >= 0 && f.sampleIndex < lastSample_) { analyzer_->reset(); lastSample_ = -1; }
    if (f.sampleIndex != lastSample_) {
        double dry[engine::kRingSize], wet[engine::kRingSize], c0[engine::kRingSize], c1[engine::kRingSize];
        for (int i = 0; i < engine::kRingSize; ++i) { c0[i] = f.transferRing[i][0]; c1[i] = f.transferRing[i][1]; }
        unrollRing(c0, engine::kRingSize, f.transferWrite, dry);
        unrollRing(c1, engine::kRingSize, f.transferWrite, wet);
        const double dt = lastSample_ < 0 ? 1.0 / 30.0 : std::clamp(static_cast<double>(f.sampleIndex - lastSample_) / fs, 1.0 / 240.0, 0.5);
        analyzer_->processFrame(dry, wet, dt);
        analyzer_->bins(bins_);
        corr_ = normalizedCorrelation(dry, wet, engine::kRingSize);
        lastSample_ = f.sampleIndex;
    }
    repaint();
}

void PhaseScatter::paint(Graphics& g) {
    paintChrome(g, "PHASE SCATTER", source_ == Source::InOut ? "dry vs wet, ch0" : "L vs R");
    g.setColour(theme_->inset);
    g.fillRect(plotArea_);
    auto f10 = font(10.0f);
    g.setFont(f10);
    const double fmax = std::min(20000.0, fs_ * 0.5);
    const float cx = plotArea_.getCentreX(), cy = plotArea_.getCentreY(), R = plotArea_.getWidth() * 0.5f - 12.0f;
    if (unavailable_.isNotEmpty()) {
        g.setColour(theme_->text);
        g.setFont(font(13.0f, true));
        g.drawText("N/A", plotArea_.withHeight(plotArea_.getHeight() * 0.5f).translated(0, plotArea_.getHeight() * 0.1f), juce::Justification::centredBottom);
        g.setColour(theme_->textDim);
        g.setFont(f10);
        g.drawFittedText(unavailable_, plotArea_.reduced(8, 0).withTrimmedTop(plotArea_.getHeight() * 0.6f).toNearestInt(), juce::Justification::centredTop, 3);
        footer_ = source_ == Source::LeftRight && unavailable_.startsWith("Not") ? "Mono processing: stereo phase not applicable." : "Use In/Out for dry-vs-wet phase of channel 0.";
    } else {
        const auto freqToX = [&](double fq) { return plotArea_.getX() + 14 + static_cast<float>(std::clamp(std::log(fq / 20.0) / std::log(fmax / 20.0), 0.0, 1.0)) * (plotArea_.getWidth() - 20); };
        const auto phaseToY = [&](double ph) { return plotArea_.getCentreY() - static_cast<float>(ph / kPi) * (plotArea_.getHeight() * 0.5f - 8); };
        if (plot_ == Plot::Polar) {
            for (double fq : {200.0, 2000.0, 20000.0}) {
                if (fq > fmax) continue;
                const float r = R * static_cast<float>(std::log(fq / 20.0) / std::log(fmax / 20.0));
                g.setColour(theme_->grid.withAlpha(0.7f));
                g.drawEllipse(cx - r, cy - r, 2 * r, 2 * r, 0.7f);
                g.setColour(theme_->textDim);
                g.drawText(fq >= 1000 ? num(fq / 1000, 0) + "k" : num(fq, 0), Rectangle<float>(cx + r - 14, cy + 1, 28, 10), juce::Justification::centred);
            }
            g.setColour(theme_->grid);
            g.drawLine(cx - R, cy, cx + R, cy, 1.0f);
            g.drawLine(cx, cy - R, cx, cy + R, 0.7f);
            g.setColour(theme_->textDim);
            g.drawText("0", Rectangle<float>(cx + R - 2, cy - 11, 14, 10), juce::Justification::centred);
            g.drawText("+90", Rectangle<float>(cx - 14, cy - R - 11, 28, 10), juce::Justification::centred);
            g.drawText("-90", Rectangle<float>(cx - 14, cy + R + 1, 28, 10), juce::Justification::centred);
            g.drawText("180", Rectangle<float>(cx - R - 14, cy - 11, 28, 10), juce::Justification::centred);
        } else {
            for (double ph : {-kPi, -kPi / 2, 0.0, kPi / 2, kPi}) {
                g.setColour(theme_->grid.withAlpha(ph == 0.0 ? 1.0f : 0.6f));
                g.drawLine(plotArea_.getX() + 14, phaseToY(ph), plotArea_.getRight() - 6, phaseToY(ph), ph == 0.0 ? 1.0f : 0.6f);
                g.setColour(theme_->textDim);
                g.drawText(num(ph * 180 / kPi, 0), Rectangle<float>(plotArea_.getX(), phaseToY(ph) - 5, 20, 10), juce::Justification::centredRight);
            }
            for (double fq : {100.0, 1000.0, 10000.0}) {
                if (fq > fmax) continue;
                g.setColour(theme_->grid.withAlpha(0.6f));
                g.drawLine(freqToX(fq), plotArea_.getY(), freqToX(fq), plotArea_.getBottom(), 0.6f);
                g.setColour(theme_->textDim);
                g.drawText(fq >= 1000 ? num(fq / 1000, 0) + "k" : num(fq, 0), Rectangle<float>(freqToX(fq) - 14, plotArea_.getBottom() - 11, 28, 10), juce::Justification::centred);
            }
        }
        const bool settled = analyzer_ && analyzer_->settled();
        int shown = 0;
        for (const auto& b : bins_) {
            if (!binVisible(b) || b.freqHz > fmax) continue;
            float x, y;
            if (plot_ == Plot::Polar) { const auto sp = scatterPoint(b.freqHz, b.phaseRad, fmax, cx, cy, R); x = static_cast<float>(sp.x); y = static_cast<float>(sp.y); }
            else { x = freqToX(b.freqHz); y = phaseToY(b.phaseRad); }
            const float size = 1.8f + 3.2f * static_cast<float>(std::clamp((b.levelDbfs + 70.0) / 70.0, 0.0, 1.0));
            const float bal = static_cast<float>(std::clamp(b.balance, -1.0, 1.0));
            const Colour c = bal >= 0 ? theme_->textDim.interpolatedWith(theme_->slotA, bal) : theme_->textDim.interpolatedWith(theme_->slotB, -bal);
            g.setColour(c.withAlpha(static_cast<float>(b.coherence) * (stale_ ? 0.35f : 1.0f) * (settled ? 1.0f : 0.6f)));
            g.fillEllipse(x - size * 0.5f, y - size * 0.5f, size, size);
            ++shown;
        }
        if (shown == 0) {
            g.setColour(theme_->textDim);
            g.drawFittedText("no bins above -70 dBFS with coherence >= 0.1", plotArea_.reduced(10).toNearestInt(), juce::Justification::centred, 3);
        }
        footer_ = "corr " + num(corr_, 2) + " (full-band) | DC L-R: not published | 256-pt window (ring-limited), tau 250 ms" +
                  (settled ? "" : " | settling") + ". Phase alone is not perceived width.";
    }
    g.setColour(theme_->border.withAlpha(0.8f));
    g.drawRect(plotArea_, 1.0f);
    auto foot = getLocalBounds().toFloat().reduced(6.0f).removeFromBottom(38.0f);
    g.setColour(theme_->textDim);
    g.drawFittedText(footer_, foot.toNearestInt(), juce::Justification::topLeft, 3, 0.9f);
}

// ---- LevelMeters -----------------------------------------------------------------------------------------
LevelMeters::LevelMeters() { setTitle("Level meters"); }

void LevelMeters::update(const engine::TelemetryFrame& f, const FrameInfo& info) {
    stale_ = info.stale || !info.valid;
    if (!info.valid) { repaint(); return; }
    fs_ = f.sampleRate > 0 ? f.sampleRate : 48000.0;
    stereo_ = info.stereo;
    const double dtMs = lastSample_ < 0 || f.sampleIndex < lastSample_ ? 0.0 : std::clamp(static_cast<double>(f.sampleIndex - lastSample_) / fs_ * 1000.0, 0.0, 500.0);
    if (f.sampleIndex != lastSample_) {
        for (int ch = 0; ch < engine::kMaxChannels; ++ch) {
            peak_[0][ch] = f.inPeak[ch]; rms_[0][ch] = f.inRms[ch];
            peak_[1][ch] = f.outPeak[ch]; rms_[1][ch] = f.outRms[ch];
            hold_[0][ch].update(f.inPeak[ch], dtMs);
            hold_[1][ch].update(f.outPeak[ch], dtMs);
        }
    }
    lastSample_ = f.sampleIndex;
    gainMatch_ = f.gainMatch;
    repaint();
}

void LevelMeters::paint(Graphics& g) {
    paintChrome(g, "LEVELS", "dBFS");
    auto area = getLocalBounds().toFloat().reduced(6.0f).withTrimmedTop(18.0f);
    const float labelH = 24.0f, topH = 16.0f;
    area.removeFromBottom(24.0f);
    const auto bars = area.withTrimmedTop(topH).withTrimmedBottom(labelH);
    const float scaleW = 22.0f;
    const double dbLo = -60.0;
    const auto yOfDb = [&](double db) { return bars.getBottom() - static_cast<float>(std::clamp((db - dbLo) / (0.0 - dbLo), 0.0, 1.0)) * bars.getHeight(); };
    g.setFont(font(9.5f));
    for (double db : {0.0, -6.0, -12.0, -24.0, -48.0}) {
        g.setColour(theme_->grid.withAlpha(0.6f));
        g.drawLine(bars.getX() + scaleW, yOfDb(db), bars.getRight(), yOfDb(db), 0.6f);
        g.setColour(theme_->textDim);
        g.drawText(num(db, 0), Rectangle<float>(bars.getX(), yOfDb(db) - 5, scaleW - 2, 10), juce::Justification::centredRight);
    }
    const float groupW = (bars.getWidth() - scaleW) / 2.0f;
    for (int grp = 0; grp < 2; ++grp) {
        const float gx = bars.getX() + scaleW + groupW * static_cast<float>(grp);
        g.setColour(theme_->text);
        g.setFont(font(10.0f, true));
        g.drawText(grp == 0 ? "IN" : "OUT", Rectangle<float>(gx, bars.getY() - topH, groupW, 12), juce::Justification::centred);
        for (int ch = 0; ch < 2; ++ch) {
            const bool dim = ch == 1 && !stereo_;
            const float bw = std::min(14.0f, groupW * 0.32f);
            const float bx = gx + groupW * 0.5f + (ch == 0 ? -bw - 2.0f : 2.0f);
            const auto track = Rectangle<float>(bx, bars.getY(), bw, bars.getHeight());
            g.setColour(theme_->inset);
            g.fillRect(track);
            const float alpha = (dim ? 0.3f : 1.0f) * (stale_ ? 0.4f : 1.0f);
            const double pk = toDb(peak_[grp][ch], -120.0), rm = toDb(rms_[grp][ch], -120.0);
            g.setColour(theme_->signal.withAlpha(0.35f * alpha));
            g.fillRect(Rectangle<float>(bx, yOfDb(pk), bw, bars.getBottom() - yOfDb(pk)));
            g.setColour(theme_->signal.withAlpha(alpha));
            g.fillRect(Rectangle<float>(bx + 3, yOfDb(rm), bw - 6, bars.getBottom() - yOfDb(rm)));
            const double hd = toDb(hold_[grp][ch].held, -120.0);
            g.setColour(theme_->text.withAlpha(alpha));
            g.fillRect(Rectangle<float>(bx - 1, yOfDb(hd) - 1, bw + 2, 2.0f));
            g.setColour(theme_->border);
            g.drawRect(track, 1.0f);
            // clip lamp: lit 2 s after a sample peak at/above full scale (square + "CLIP" text colour-independent)
            const auto lamp = Rectangle<float>(bx, bars.getY() - 5.0f, bw, 4.0f);
            g.setColour(hold_[grp][ch].clipLit() ? theme_->danger : theme_->grid);
            g.fillRect(lamp);
            g.setFont(font(9.5f));
            g.setColour(theme_->textDim.withAlpha(dim ? 0.5f : 1.0f));
            g.drawText(ch == 0 ? "L" : "R", Rectangle<float>(bx, bars.getBottom() + 1, bw, 10), juce::Justification::centred);
            g.setColour(theme_->text.withAlpha(dim ? 0.5f : 1.0f));
            g.drawText(dim ? "--" : num(pk, 1), Rectangle<float>(bx - 4, bars.getBottom() + 11, bw + 8, 11), juce::Justification::centred);
        }
    }
    g.setFont(font(10.0f));
    g.setColour(theme_->textDim);
    const juce::String gm = "peak (light) / RMS (solid) / hold 1 s; clip lamp 2 s. Gain match " + signedNum(toDb(gainMatch_, -60.0), 1) + " dB";
    g.drawFittedText(gm, getLocalBounds().toFloat().reduced(6.0f).removeFromBottom(24.0f).toNearestInt(), juce::Justification::topLeft, 2, 0.9f);
}

}  // namespace cd::plugin::ui::analysis
