// Workbench view (CD-F17 inspector, UX-01 gestures, UX-04/05/06 displays). Owns the toolbar, the
// PCB canvas, the inspector and the four analysis displays; receives one telemetry frame per UI
// frame from the editor (no timers of its own, no audio coupling).
#include "ui/workbench/WorkbenchView.h"

#include "ui/analysis/AnalysisDisplays.h"
#include "ui/workbench/InspectorPanel.h"
#include "ui/workbench/PcbCanvas.h"

namespace cd::plugin::ui {

struct WorkbenchView::Impl {
    Impl(WorkbenchView& o, EditorBridge& b) : owner(o), bridge(b), canvas(b), inspector(b, canvas) {
        canvas.setComponentID("pcb");
        inspector.setComponentID("inspector");
        scope.setComponentID("scope");
        timePlot.setComponentID("timeplot");
        phase.setComponentID("phase");
        meters.setComponentID("meters");
        for (juce::Component* c : std::initializer_list<juce::Component*>{&canvas, &inspector, &scope, &timePlot, &phase, &meters}) owner.addAndMakeVisible(c);

        struct Def { juce::TextButton* b; const char* text; const char* title; int radio; };
        const Def defs[] = {
            {&overlay[0], "Off", "Overlay off", 7301}, {&overlay[1], "Therm", "Thermal camera overlay", 7301},
            {&overlay[2], "Volt", "Voltage heat map overlay", 7301}, {&overlay[3], "Sig", "Signal flow overlay", 7301},
            {&viewBtn[0], "2D", "Flat 2D board view", 7302}, {&viewBtn[1], "3D", "Oblique 3D board view (orbit with drag)", 7302},
            {&viewBtn[2], "Sch", "Accessible 2D schematic view", 7302},
            {&toolBtn[0], "Auto", "Tool: pick by target (tube bias, capacitor supply, resistor age brush)", 7303},
            {&toolBtn[1], "Bias", "Tool: tube bias drag", 7303}, {&toolBtn[2], "Supply", "Tool: supply capacity (drag, scroll or pinch)", 7303},
            {&toolBtn[3], "Age", "Tool: resistor age brush", 7303}, {&toolBtn[4], "Mat", "Tool: resistor material selector (separate from Age)", 7303},
            {&chBtn[0], "Ch1", "Show channel 1 telemetry", 7304}, {&chBtn[1], "Ch2", "Show channel 2 telemetry", 7304},
        };
        for (const auto& d : defs) {
            d.b->setButtonText(d.text);
            d.b->setTitle(d.title);
            d.b->setTooltip(d.title);
            d.b->setRadioGroupId(d.radio);
            d.b->setClickingTogglesState(true);
            owner.addAndMakeVisible(d.b);
        }
        xrayBtn.setButtonText("X-Ray");
        xrayBtn.setTitle("X-Ray: ghost the components and reveal hidden traces and values");
        xrayBtn.setTooltip(xrayBtn.getTitle());
        xrayBtn.setClickingTogglesState(true);
        owner.addAndMakeVisible(xrayBtn);
        inspBtn.setButtonText("Info");
        inspBtn.setTitle("Show or hide the inspector drawer");
        inspBtn.setClickingTogglesState(true);
        owner.addChildComponent(inspBtn);

        for (int i = 0; i < 4; ++i) overlay[i].onClick = [this, i] {
            auto s = bridge.uiSettings(); s.overlay = i; bridge.setUiSettings(s); apply(s);
        };
        xrayBtn.onClick = [this] { auto s = bridge.uiSettings(); s.xray = xrayBtn.getToggleState(); bridge.setUiSettings(s); apply(s); };
        for (int i = 0; i < 3; ++i) viewBtn[i].onClick = [this, i] {
            auto s = bridge.uiSettings();
            s.pcbView = i;
            bridge.setUiSettings(s);
            apply(s);
        };
        for (int i = 0; i < 5; ++i) toolBtn[i].onClick = [this, i] { canvas.setTool(static_cast<PcbCanvas::Tool>(i)); };
        for (int i = 0; i < 2; ++i) chBtn[i].onClick = [this, i] { canvas.setChannel(i); refreshAll(); };
        inspBtn.onClick = [this] { drawerOpen = inspBtn.getToggleState(); owner.resized(); };
        canvas.onSelect = [this](int id) {
            if (id >= 0 && narrow()) { drawerOpen = true; inspBtn.setToggleState(true, juce::dontSendNotification); owner.resized(); }
            refreshAll();
        };
        canvas.onStateChanged = [this] { syncButtons(); };
        apply(bridge.uiSettings());
        syncButtons();
    }

    bool narrow() const { return owner.getWidth() < 900; }

    void apply(const UiSettings& s) {
        settings = s;
        const Theme& t = themeFor(s.darkTheme);
        canvas.applySettings(s);
        inspector.setTheme(t);
        inspector.setEducational(s.educational);
        for (auto* d : std::initializer_list<analysis::DisplayBase*>{&scope, &timePlot, &phase, &meters}) d->setTheme(t);
        styleButtons(t);
        syncButtons();
        owner.repaint();
    }

    void styleButtons(const Theme& t) {
        for (auto* b : allButtons()) {
            b->setColour(juce::TextButton::buttonColourId, t.panelRaised);
            b->setColour(juce::TextButton::buttonOnColourId, t.accent);
            b->setColour(juce::TextButton::textColourOffId, t.text);
            b->setColour(juce::TextButton::textColourOnId, t.textInverse);
        }
    }

    std::vector<juce::TextButton*> allButtons() {
        std::vector<juce::TextButton*> v;
        for (auto& b : overlay) v.push_back(&b);
        for (auto& b : viewBtn) v.push_back(&b);
        for (auto& b : toolBtn) v.push_back(&b);
        for (auto& b : chBtn) v.push_back(&b);
        v.push_back(&xrayBtn);
        v.push_back(&inspBtn);
        return v;
    }

    void syncButtons() {
        for (int i = 0; i < 4; ++i) overlay[i].setToggleState(i == std::clamp(settings.overlay, 0, 3), juce::dontSendNotification);
        xrayBtn.setToggleState(settings.xray, juce::dontSendNotification);
        for (int i = 0; i < 3; ++i) viewBtn[i].setToggleState(i == static_cast<int>(canvas.viewMode()), juce::dontSendNotification);
        for (int i = 0; i < 5; ++i) toolBtn[i].setToggleState(i == static_cast<int>(canvas.tool()), juce::dontSendNotification);
        for (int i = 0; i < 2; ++i) chBtn[i].setToggleState(i == canvas.channel(), juce::dontSendNotification);
    }

    analysis::FrameInfo makeInfo(const engine::TelemetryFrame& f, bool valid) {
        analysis::FrameInfo info;
        info.valid = valid;
        info.stale = canvas.stale();
        info.slot = canvas.editSlot();
        info.channel = canvas.channel();
        info.selected = canvas.selected();
        info.ambientC = canvas.effective().ambientK - 273.15;
        info.bpm = bridge.status().bpm;
        info.reduceMotion = settings.reduceMotion;
        // Mono detection from the published levels: a processing path with no second channel never
        // reports energy on it (an all-silent second channel is treated as mono, documented).
        info.stereo = f.inPeak[1] > 0.0 || f.outPeak[1] > 0.0;
        info.nowMs = juce::Time::getMillisecondCounterHiRes();
        return info;
    }

    void refreshAll() {
        const auto info = makeInfo(canvas.frame(), canvas.frameValid());
        scope.update(canvas.frame(), info);
        timePlot.update(canvas.frame(), info);
        phase.update(canvas.frame(), info);
        meters.update(canvas.frame(), info);
        inspector.refresh();
    }

    void layout() {
        auto r = owner.getLocalBounds().reduced(8);
        auto bar = r.removeFromTop(28);
        r.removeFromTop(6);
        const bool nar = narrow();
        inspBtn.setVisible(nar);
        // Toolbar: 16 equal-weight buttons with small group gaps.
        const int gaps = nar ? 5 : 4;
        const int nb = 4 + 1 + 3 + 5 + 2 + (nar ? 1 : 0);
        const int bw = std::max(30, (bar.getWidth() - gaps * 8) / nb);
        auto place = [&](juce::TextButton* b, int n) { for (int i = 0; i < n; ++i) { b[i].setBounds(bar.removeFromLeft(bw).reduced(1, 0)); } bar.removeFromLeft(8); };
        place(overlay, 4);
        xrayBtn.setBounds(bar.removeFromLeft(bw + 6).reduced(1, 0)); bar.removeFromLeft(8);
        place(viewBtn, 3);
        place(toolBtn, 5);
        place(chBtn, 2);
        if (nar) inspBtn.setBounds(bar.removeFromLeft(bw).reduced(1, 0));

        const int inspW = nar ? 0 : std::clamp(static_cast<int>(r.getWidth() * 0.27f), 230, 310);
        auto inspArea = r.removeFromRight(inspW);
        if (inspW > 0) r.removeFromRight(8);
        const int stripH = std::clamp(static_cast<int>(r.getHeight() * 0.42f), 150, 250);
        auto strip = r.removeFromBottom(stripH);
        r.removeFromBottom(8);
        canvas.setBounds(r);
        if (nar) {
            inspector.setBounds(r.removeFromRight(std::min(260, r.getWidth() * 2 / 3)).reduced(0));
            inspector.setVisible(drawerOpen);
            inspector.toFront(false);
        } else {
            inspector.setBounds(inspArea.withTop(r.getY()).withBottom(strip.getBottom()));
            inspector.setVisible(true);
        }
        const float total = 1.0f + 1.25f + 1.0f + 0.8f;
        const int gap = 6, usable = strip.getWidth() - 3 * gap;
        auto col = [&](float f) { return strip.removeFromLeft(static_cast<int>(usable * f / total)); };
        scope.setBounds(col(1.0f)); strip.removeFromLeft(gap);
        timePlot.setBounds(col(1.25f)); strip.removeFromLeft(gap);
        phase.setBounds(col(1.0f)); strip.removeFromLeft(gap);
        meters.setBounds(strip);
    }

    WorkbenchView& owner;
    EditorBridge& bridge;
    UiSettings settings;
    PcbCanvas canvas;
    InspectorPanel inspector;
    analysis::TransferScope scope;
    analysis::TimePlot timePlot;
    analysis::PhaseScatter phase;
    analysis::LevelMeters meters;
    juce::TextButton overlay[4], viewBtn[3], toolBtn[5], chBtn[2], xrayBtn, inspBtn;
    bool drawerOpen = false;
};

WorkbenchView::WorkbenchView(EditorBridge& bridge) : impl_(std::make_unique<Impl>(*this, bridge)) {
    setTitle("Workbench");
}
WorkbenchView::~WorkbenchView() = default;

void WorkbenchView::updateFrame(const engine::TelemetryFrame& frame, bool frameValid) {
    auto& i = *impl_;
    i.canvas.ingest(frame, frameValid, juce::Time::getMillisecondCounterHiRes());
    const auto info = i.makeInfo(i.canvas.frame(), frameValid);
    i.scope.update(i.canvas.frame(), info);
    i.timePlot.update(i.canvas.frame(), info);
    i.phase.update(i.canvas.frame(), info);
    i.meters.update(i.canvas.frame(), info);
    i.inspector.refresh();
}

void WorkbenchView::settingsChanged(const UiSettings& settings) { impl_->apply(settings); }

void WorkbenchView::resized() { impl_->layout(); }

void WorkbenchView::paint(juce::Graphics& g) {
    g.fillAll(themeFor(impl_->settings.darkTheme).background);
}

}  // namespace cd::plugin::ui
