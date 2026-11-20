// Lifecycle (CD-F18 / UX-03), Faults (CD-F19) and Blueprint (CD-F10) panels.
#include <cmath>
#include "ui/panels/PanelCommon.h"
#include "ui/workbench/GestureMath.h"

namespace cd::plugin::ui::pc {
namespace {

// ----------------------------------------------------------------------------------- Lifecycle
struct Scenario {
    const char* label;
    int years[4];
    const char* names[4];
    bool restored;
};
constexpr Scenario kScenarios[3] = {
    {"Derelict ending, 1975 set", {1975, 1990, 2010, 2026}, {"Factory Spec", "Tour-Worn", "Attic Storage", "Derelict"}, false},
    {"Derelict ending, 1972 set", {1972, 1985, 2005, 2026}, {"Factory Spec", "Tour-Worn", "Attic Storage", "Derelict"}, false},
    {"Restored ending, 1975 set", {1975, 1985, 2005, 2026}, {"Factory Spec", "Tour-Worn", "Attic Storage", "Restored"}, true}};

struct Anchor { double age, c, esr, leak, sigma, bias, noiseDb, drift; };
constexpr Anchor kAnchors[4] = {{0, 1.0, 1.0, 0.0, 0.0033, 0.0, 0.0, 1.0},
                                {15, 0.95, 1.4, 0.01, 0.02, 0.015, 3.0, 1.5},
                                {35, 0.8, 3.0, 0.08, 0.05, 0.04, 9.0, 3.0},
                                {51, 0.65, 6.0, 0.2, 0.08, 0.08, 15.0, 5.0}};
constexpr Anchor kRestoredEnd = {5, 1.0, 1.0, 0.0, 0.0033, 0.0, 0.0, 1.0};

struct Condition { double year, age, c, esr, leak, noiseDb, drift; int seg; double t; };

Condition interpolate(double u, const Scenario& sc) {
    u = juce::jlimit(0.0, 1.0, u);
    const int seg = juce::jmin(2, static_cast<int>(std::floor(u * 3.0)));
    const double t = u * 3.0 - seg;
    const double h = t * t * (3.0 - 2.0 * t);  // smoothstep per spec
    const Anchor& a = kAnchors[seg];
    const Anchor& b = (seg == 2 && sc.restored) ? kRestoredEnd : kAnchors[seg + 1];
    auto lin = [h](double x, double y) { return x + (y - x) * h; };
    auto lg = [h](double x, double y) { return std::exp(std::log(x) + (std::log(y) - std::log(x)) * h); };
    Condition c{};
    c.seg = seg; c.t = t;
    c.year = sc.years[seg] + (sc.years[seg + 1] - sc.years[seg]) * t;  // linear between label years
    c.age = lin(a.age, b.age);
    c.c = lg(a.c, b.c);
    c.esr = lg(a.esr, b.esr);
    c.leak = lin(a.leak, b.leak);
    c.noiseDb = lin(a.noiseDb, b.noiseDb);
    c.drift = lg(a.drift, b.drift);
    return c;
}

class AnchorStrip : public juce::Component {
public:
    AnchorStrip() { setInterceptsMouseClicks(false, false); setTitle("Lifecycle anchors"); }
    void setState(double u, int scenario, int textBoxW) {
        u_ = u; scenario_ = juce::jlimit(0, 2, scenario); textBoxW_ = textBoxW;
        repaint();
    }
    static constexpr int kHeight = 46;
    void paint(juce::Graphics& g) override {
        const Theme& t = themeOf(*this);
        const float inset = 11.0f;
        const float left = inset, right = static_cast<float>(getWidth() - textBoxW_) - inset;
        const auto& sc = kScenarios[scenario_];
        g.setFont(juce::Font(juce::FontOptions(11.5f)));
        for (int i = 0; i < 4; ++i) {
            const float x = left + (right - left) * static_cast<float>(i) / 3.0f;
            const bool near = std::abs(u_ - i / 3.0) < 0.02;
            g.setColour(near ? t.accent : t.border);
            g.fillRect(x - 1.0f, 0.0f, 2.0f, 8.0f);
            g.setColour(near ? t.text : t.textDim);
            const float w = (right - left) / 3.0f;
            auto box = juce::Rectangle<float>(x - w * 0.5f, 10.0f, w, 34.0f);
            auto just = juce::Justification::centredTop;
            if (i == 0) { box = juce::Rectangle<float>(0.0f, 10.0f, w, 34.0f); just = juce::Justification::topLeft; }
            if (i == 3) { box = juce::Rectangle<float>(right + inset - w, 10.0f, w, 34.0f); just = juce::Justification::topRight; }
            g.drawFittedText(juce::String(sc.years[i]) + "\n" + sc.names[i], box.toNearestInt(), just, 2, 0.85f);
        }
    }

private:
    double u_ = 0;
    int scenario_ = 0, textBoxW_ = 84;
};

class LifecyclePanel final : public PanelBase {
public:
    explicit LifecyclePanel(EditorBridge& b)
        : PanelBase(b),
          scrub_(b, "Hardware Lifecycle", "Scrub hardware condition from factory-fresh to the scenario ending; reversible, one drag is one undo step"),
          scenario_(b, "Scenario", "Choose which authored set of dates and ending the scrubber follows"),
          derelict_("Derelict outcome"), restored_("Restored outcome") {
        setTitle("Lifecycle");
        heading("Hardware Lifecycle");
        note("Authored condition profiles, not a measurement of real hardware age. Scrubbing previews a reversible condition target and never accumulates damage.", Tone::Dim);
        content_.addItem(scenario_, 240, ParamChoice::kHeight, true);
        for (auto* t : {&derelict_, &restored_}) { t->setClickingTogglesState(true); t->setRadioGroupId(7101); }
        tip(derelict_, "Derelict outcome", "Select the scenario that ends in worn-out, derelict condition");
        tip(restored_, "Restored outcome", "Select the scenario that ends with replacement components (Restored)");
        derelict_.onClick = [this] { if (derelict_.getToggleState()) scenario_.binding.setComplete(0.0f); };
        restored_.onClick = [this] { if (restored_.getToggleState()) scenario_.binding.setComplete(2.0f); };
        content_.addItem(derelict_, buttonBasis("Derelict outcome"), 30);
        content_.addItem(restored_, buttonBasis("Restored outcome"), 30);
        scrub_.setDisplay([](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; }, gesture::parsePercentage);
        scrub_.slider.setNumDecimalPlacesToDisplay(0);
        content_.addItem(scrub_, 260, ParamSlider::kHeight, true);
        content_.addItem(strip_, 260, AnchorStrip::kHeight, true);
        year_ = note("", Tone::Normal);
        cond_ = note("", Tone::Normal);
        heading("Restoration");
        commit_ = makeButton("Commit Restoration", "Replace aged components once: changes replacement seeds at the Restored ending (one undo step)");
        commit_->onClick = [this] {
            auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Commit Restoration",
                                                                     "Commit replacement of aged components? Replacement seeds change once. This is one undo step.",
                                                                     "Commit", "Cancel", this);
            juce::Component::SafePointer<LifecyclePanel> safe(this);
            juce::AlertWindow::showAsync(opts, [safe](int r) {
                if (safe != nullptr && r == 1) safe->bridge.commitRestoration(safe->bridge.status().editSlot);
            });
        };
        content_.addItem(*commit_, buttonBasis("Commit Restoration"), 30);
        commitNote_ = note("", Tone::Dim);
        heading("Condition target vs live temperature");
        live_ = note("", Tone::Normal);
        note("The condition target follows the scrubber at once (smoothed 50 ms). Live temperature moves over seconds of thermal inertia and does not jump to the target.");
        rebind();
    }

    void refresh(const StatusInfo& s) override {
        if (bridge.slotParamId("lifecycle") != scrub_.binding.id()) rebind();
        const double u = scrub_.binding.bound() ? scrub_.binding.value() : 0.0;
        const int sci = scenario_.binding.bound() ? juce::jlimit(0, 2, scenario_.index()) : 0;
        strip_.setState(u, sci, scrub_.slider.getTextBoxWidth());
        derelict_.setToggleState(!kScenarios[sci].restored, juce::dontSendNotification);
        restored_.setToggleState(kScenarios[sci].restored, juce::dontSendNotification);
        const auto& sc = kScenarios[sci];
        const auto c = interpolate(u, sc);
        bool ch = false;
        const int seg = c.seg;
        const juce::String between = c.t < 0.02 ? juce::String("at ") + sc.names[seg] : c.t > 0.98 ? juce::String("at ") + sc.names[seg + 1]
                                                                                                  : juce::String("between ") + sc.names[seg] + " and " + sc.names[seg + 1];
        ch |= year_->set("Display year " + juce::String(juce::roundToInt(c.year)) + " (" + between + "), lifecycle " + juce::String(juce::roundToInt(u * 100.0)) + " %" +
                             ". Outcome at 100 %: " + (sc.restored ? "Restored (replacement parts)" : "Derelict"),
                         Tone::Normal);
        ch |= cond_->set("Condition target: capacitance x" + juce::String(c.c, 2) + ", ESR x" + juce::String(c.esr, 1) + ", cap leakage " +
                             juce::String(c.leak * 100.0, 1) + " % of safe span, added noise +" + juce::String(c.noiseDb, 1) + " dB (only audible when Analog Noise is on), drift x" +
                             juce::String(c.drift, 1) + ". Authored profile, not measured.",
                         Tone::Dim);
        const bool supported = bridge.supportsRestorationCommit();
        const bool canCommit = supported && sc.restored && u >= 0.999;
        commit_->setEnabled(canCommit);
        ch |= commitNote_->set(!supported ? "Component replacement is unavailable until runtime seed transactions are connected (G12)."
                                       : canCommit ? "Ready: the Restored ending is selected and the scrubber is at 100 %."
                                         : "Available only with the Restored scenario at 100 %. Previewing never reseeds components.",
                               canCommit ? Tone::Ok : Tone::Dim);
        engine::TelemetryFrame f;
        juce::String live = "Live temperature: no data yet.";
        if (bridge.readTelemetry(f)) {
            const int slot = juce::jlimit(0, engine::kSlots - 1, s.editSlot);
            double hot = -1e9; int hi = 0;
            for (int i = 0; i < engine::kComponentCount; ++i) {
                const double k = f.slot[slot].temperatureK[0][i];
                if (k > hot) { hot = k; hi = i; }
            }
            live = "Live temperature (slot " + juce::String(slot == 0 ? "A" : "B") + ", channel 1): hottest " + juce::String(hot - 273.15, 1) + " C at " +
                   engine::componentTable()[hi].key + ". Separate from the condition target above.";
        }
        ch |= live_->set(live, Tone::Normal);
        if (ch) relayout();
    }

private:
    void rebind() {
        scrub_.bind(bridge.slotParamId("lifecycle"));
        scrub_.slider.updateText();
        scenario_.bind(bridge.slotParamId("scenario"), {kScenarios[0].label, kScenarios[1].label, kScenarios[2].label});
    }
    ParamSlider scrub_;
    ParamChoice scenario_;
    juce::TextButton derelict_, restored_;
    AnchorStrip strip_;
    std::unique_ptr<juce::TextButton> commit_;
    NoteLabel *year_ = nullptr, *cond_ = nullptr, *commitNote_ = nullptr, *live_ = nullptr;
};

// -------------------------------------------------------------------------------------- Faults
enum class Family { None, Electrolytic, Resistor, Tube };

Family familyOf(engine::ComponentRole r) {
    using R = engine::ComponentRole;
    switch (r) {
        case R::CathodeBypass: case R::OutputCoupling: case R::Decoupling: case R::Reservoir: case R::FilterCapacitor: return Family::Electrolytic;
        case R::GridLeakBias: case R::PlateLoad: case R::CathodeResistor: case R::DecouplingResistor: case R::ScreenResistor: return Family::Resistor;
        case R::ActiveElement: return Family::Tube;
        default: return Family::None;
    }
}

void configurePlain(ParamSlider& ps, double lo, double hi, double step, const juce::String& suffix, double init) {
    ps.slider.setRange(lo, hi, step);
    ps.slider.setValue(init, juce::dontSendNotification);
    ps.slider.onValueChange = nullptr;
    const int digits = step >= 1.0 ? 0 : step >= 0.1 ? 1 : 2;
    ps.slider.textFromValueFunction = [suffix, digits](double v) { return juce::String(v, digits) + suffix; };
    ps.slider.valueFromTextFunction = [&slider = ps.slider, lo, hi, unit = suffix.trim().toStdString()](const juce::String& t) {
        if (t.trim() == slider.getTextFromValue(slider.getValue()).trim()) return slider.getValue();
        return gesture::parseBoundedQuantity(t.toStdString(), unit, lo, hi).value_or(slider.getValue());
    };
    ps.slider.updateText();
}

class FaultsPanel final : public PanelBase {
public:
    explicit FaultsPanel(EditorBridge& b)
        : PanelBase(b),
          component_(b, "Component", "Circuit component to fault (engine component key)"),
          type_(b, "Fault type", "Only fault types valid for this component are offered"),
          severity_(b, "Severity", "How far the fault goes, 0 to 100 percent"),
          hz_(b, "Resonance frequency", "Microphonic mode frequency, 100 to 4000 Hz"),
          q_(b, "Resonance Q", "Microphonic mode quality factor, 2 to 30"),
          intermittent_(b, "Intermittent contact", "Seeded, rate-limited intermittent contact (cracked resistor only)") {
        setTitle("Faults");
        heading("Fault injector");
        note("Artistic bounded failure simulation. These are not validated failure models: output stays finite and bounded, and containment can never be disabled. Faults are reversible and part of undo, A/B and presets.", Tone::Warn);
        const auto* tbl = engine::componentTable();
        for (int i = 0; i < engine::kComponentCount; ++i) {
            // Keep the key and human name readable at the supported 360 px drawer width.
            // Detailed model qualifiers remain in the selected component's tooltip and notes.
            const auto name = juce::String(tbl[i].displayName).upToFirstOccurrenceOf(" (", false, false);
            component_.box.addItem(juce::String(tbl[i].key) + " - " + name, i + 1);
        }
        component_.box.setSelectedId(engine::kComponentCount > 5 ? static_cast<int>(engine::ComponentId::C_k) + 1 : 1, juce::dontSendNotification);
        component_.box.onChange = [this] { rebuildTypes(); };
        content_.addItem(component_, 260, ParamChoice::kHeight, true);
        type_.box.onChange = [this] { updatePreview(); };
        content_.addItem(type_, 260, ParamChoice::kHeight, true);
        configurePlain(severity_, 0.0, 1.0, 0.01, "", 0.5);
        severity_.slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
        severity_.slider.valueFromTextFunction = [this](const juce::String& t) {
            return gesture::parsePercentage(t.toStdString()).value_or(severity_.slider.getValue());
        };
        severity_.slider.onValueChange = [this] { updatePreview(); };
        severity_.slider.updateText();
        content_.addItem(severity_, 260, ParamSlider::kHeight, true);
        configurePlain(hz_, 100.0, 4000.0, 1.0, " Hz", 800.0);
        configurePlain(q_, 2.0, 30.0, 0.1, "", 8.0);
        hz_.slider.setSkewFactorFromMidPoint(800.0);
        hz_.slider.onValueChange = [this] { updatePreview(); };
        q_.slider.onValueChange = [this] { updatePreview(); };
        content_.addItem(hz_, 240, ParamSlider::kHeight);
        content_.addItem(q_, 240, ParamSlider::kHeight);
        intermittent_.onUserChange = [this](bool) { updatePreview(); };
        intermittent_.setEnabled(true);
        content_.addItem(intermittent_, 240, ParamToggle::kHeight, true);
        preview_ = note("", Tone::Normal);
        reason_ = note("", Tone::Warn);
        apply_ = makeButton("Apply fault", "Apply the previewed fault to the edit slot (reversible, one undo step)");
        clear_ = makeButton("Clear all faults", "Remove every fault overlay on the edit slot with a 20 ms ramp");
        apply_->onClick = [this] { applyFault(); };
        clear_->onClick = [this] { bridge.clearFaults(bridge.status().editSlot); poll(true); };
        content_.addItem(*apply_, buttonBasis("Apply fault"), 30);
        content_.addItem(*clear_, buttonBasis("Clear all faults"), 30);
        error_ = note("", Tone::Danger);
        heading("Active faults");
        active_ = note("", Tone::Normal);
        rebuildTypes();
        poll(true);
    }
    void refresh(const StatusInfo& s) override {
        if (s.editSlot != slot_) { slot_ = s.editSlot; rebuildTypes(); }
        poll(false);
    }

private:
    const engine::ComponentInfo& comp() const { return engine::componentTable()[juce::jlimit(0, engine::kComponentCount - 1, component_.box.getSelectedId() - 1)]; }
    int typeCode() const { return type_.box.getSelectedId(); }  // FaultType value + 1000 offset
    void rebuildTypes() {
        const auto& c = comp();
        component_.box.setTooltip(juce::String(c.key) + " - " + c.displayName + ". Circuit component to fault.");
        component_.box.setDescription(component_.box.getTooltip());
        const auto fam = familyOf(c.role);
        type_.box.clear(juce::dontSendNotification);
        reasonText_ = {};
        if (fam == Family::Electrolytic) {
            type_.box.addItem("Electrolytic open", 1000 + static_cast<int>(FaultType::ElectrolyticOpen));
            type_.box.addItem("Electrolytic leaky / short", 1000 + static_cast<int>(FaultType::ElectrolyticLeaky));
        } else if (fam == Family::Resistor) {
            const auto* p = bridge.parameters().getParameter(bridge.slotParamId("condition.resistorType"));
            const bool carbon = p == nullptr || std::lround(p->convertFrom0to1(p->getValue())) == 0;
            if (carbon) type_.box.addItem("Cracked carbon resistor", 1000 + static_cast<int>(FaultType::CrackedResistor));
            else reasonText_ = "Cracked-resistor faults apply to Carbon composition resistors only; the edit slot uses another resistor type.";
        } else if (fam == Family::Tube) {
            const auto* p = bridge.parameters().getParameter(bridge.slotParamId("stage.activeElement"));
            const long el = p ? std::lround(p->convertFrom0to1(p->getValue())) : 0;
            if (el <= 1) type_.box.addItem("Microphonic tube", 1000 + static_cast<int>(FaultType::MicrophonicTube));
            else reasonText_ = "Microphonic faults need a tube element; the edit slot uses a transistor family.";
        } else {
            reasonText_ = juce::String(c.displayName) + " has no fault model. Choose a capacitor, carbon resistor or the tube.";
        }
        if (type_.box.getNumItems() > 0) type_.box.setSelectedItemIndex(0, juce::dontSendNotification);
        type_.box.setEnabled(type_.box.getNumItems() > 0);
        updatePreview();
    }
    void updatePreview() {
        const auto ft = static_cast<FaultType>(typeCode() >= 1000 ? typeCode() - 1000 : 0);
        const bool mic = ft == FaultType::MicrophonicTube, crack = ft == FaultType::CrackedResistor;
        hz_.setVisible(mic);
        q_.setVisible(mic);
        intermittent_.setVisible(crack);
        const int sev = juce::roundToInt(severity_.slider.getValue() * 100.0);
        juce::String t;
        const juce::String key = comp().key;
        switch (ft) {
            case FaultType::ElectrolyticOpen: t = "Preview: " + key + " loses capacitance toward the model minimum while its series resistance rises toward a bounded open-equivalent over 10 ms (severity " + juce::String(sev) + " %). Expect thin, hum-prone, bass-light tone; never an ideal infinity."; break;
            case FaultType::ElectrolyticLeaky: t = "Preview: leakage across " + key + " grows toward a bounded short-equivalent (severity " + juce::String(sev) + " %). Expect shifted bias, DC wander and a squashed, noisy sound. Current limiting stays active."; break;
            case FaultType::CrackedResistor: t = "Preview: " + key + " rises toward a bounded open-equivalent with 2 ms transitions (severity " + juce::String(sev) + " %)" + (intermittent_.getToggleState() ? ", with seeded intermittent contact limited to 20 transitions per second." : "."); break;
            case FaultType::MicrophonicTube: t = "Preview: a damped resonance at " + juce::String(juce::roundToInt(hz_.slider.getValue())) + " Hz, Q " + juce::String(q_.slider.getValue(), 1) + " is excited by transients (severity " + juce::String(sev) + " %), limited to 5 % of the bias span. A creative surrogate, not a measured tube."; break;
            default: t = "Nothing to preview."; break;
        }
        bool ch = preview_->set(t, Tone::Normal);
        ch |= reason_->set(reasonText_, Tone::Warn);
        apply_->setEnabled(ft != FaultType::None);
        ch |= error_->set("");
        if (ch || true) relayout();
    }
    void applyFault() {
        const auto ft = static_cast<FaultType>(typeCode() >= 1000 ? typeCode() - 1000 : 0);
        if (ft == FaultType::None) return;
        FaultInfo f;
        f.componentKey = comp().key;
        f.type = ft;
        f.severity = severity_.slider.getValue();
        f.enabled = true;
        f.seed = 0x5EEDull + static_cast<std::uint64_t>(shown_.size()) * 7919ull;
        f.intermittent = ft == FaultType::CrackedResistor && intermittent_.getToggleState();
        f.modeHz = hz_.slider.getValue();
        f.modeQ = q_.slider.getValue();
        juce::String err;
        if (!bridge.applyFault(bridge.status().editSlot, f, err)) { error_->set("Could not apply: " + err); relayout(); return; }
        error_->set("");
        poll(true);
    }
    static juce::String typeName(FaultType t) {
        switch (t) {
            case FaultType::ElectrolyticOpen: return "Electrolytic open";
            case FaultType::ElectrolyticLeaky: return "Electrolytic leaky";
            case FaultType::CrackedResistor: return "Cracked resistor";
            case FaultType::MicrophonicTube: return "Microphonic tube";
            default: return "None";
        }
    }
    void poll(bool force) {
        const auto list = bridge.faults(bridge.status().editSlot);
        juce::String text;
        for (const auto& f : list) {
            if (!f.enabled || f.type == FaultType::None) continue;
            text << f.componentKey << ": " << typeName(f.type) << ", severity " << juce::roundToInt(f.severity * 100.0) << " %";
            if (f.intermittent) text << ", intermittent";
            if (f.type == FaultType::MicrophonicTube) text << ", " << juce::roundToInt(f.modeHz) << " Hz, Q " << juce::String(f.modeQ, 1);
            text << "\n";
        }
        shown_ = list;
        const bool any = text.isNotEmpty();
        clear_->setEnabled(any);
        if (active_->set(any ? text.trimEnd() : juce::String("No active faults. All overlays are off."), any ? Tone::Warn : Tone::Dim) || force) relayout();
    }

    ParamChoice component_, type_;
    ParamSlider severity_, hz_, q_;
    ParamToggle intermittent_;
    std::unique_ptr<juce::TextButton> apply_, clear_;
    NoteLabel *preview_ = nullptr, *reason_ = nullptr, *error_ = nullptr, *active_ = nullptr;
    juce::String reasonText_;
    std::vector<FaultInfo> shown_;
    int slot_ = 0;
};

// ------------------------------------------------------------------------------------ Blueprint
enum class BpState { Idle, Draft, Validating, Ready, Invalid, Applying };

class BlueprintPanel final : public PanelBase {
public:
    explicit BlueprintPanel(EditorBridge& b) : PanelBase(b) {
        setTitle("Blueprint");
        heading("Circuit blueprint");
        state_ = note("", Tone::Normal);
        editor_.setMultiLine(true, true);
        editor_.setReturnKeyStartsNewLine(true);
        editor_.setScrollbarsShown(true);
        editor_.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
        tip(editor_, "Blueprint JSON", "Blueprint of the edit slot as JSON; edit, then Validate before Apply");
        editor_.onTextChange = [this] { if (!loading_) setState(BpState::Draft); };
        content_.addItem(editor_, 240, 240, true);
        validate_ = makeButton("Validate", "Check the blueprint on a worker thread without changing the sound");
        reload_ = makeButton("Reload", "Discard edits and reload the blueprint of the edit slot");
        apply_ = makeButton("Apply...", "Apply the validated blueprint to the edit slot (asks first)");
        validate_->onClick = [this] { startValidation(); };
        reload_->onClick = [this] { load(); };
        apply_->onClick = [this] { confirmApply(); };
        content_.addItem(*validate_, buttonBasis("Validate"), 30);
        content_.addItem(*reload_, buttonBasis("Reload"), 30);
        content_.addItem(*apply_, buttonBasis("Apply..."), 30);
        heading("Report");
        counts_ = note("", Tone::Normal);
        diag_ = note("", Tone::Normal);
        note("Validation never changes the audio. Apply swaps the circuit only after it compiled; containment and dry fallback stay on. Applying is one undo step.");
        if (!bridge.supportsBlueprintApply())
            note("Validation preview is available. Applying a custom circuit to live audio is not available yet.", Tone::Dim);
        load();
    }

    void refresh(const StatusInfo& s) override {
        if (s.editSlot != slot_ && state_v_ != BpState::Draft) { slot_ = s.editSlot; load(); }
        slot_ = s.editSlot;
        if (state_v_ == BpState::Validating) {
            BlueprintReport r;
            if (bridge.blueprintReport(r)) {
                report_ = r;
                hasReport_ = true;
                const bool stale = editor_.getText() != validatedText_;
                showReport();
                setState(stale ? BpState::Draft : (r.ok ? BpState::Ready : BpState::Invalid));
            }
        }
    }

private:
    void load() {
        loading_ = true;
        editor_.setText(bridge.blueprintJson(bridge.status().editSlot), false);
        loading_ = false;
        hasReport_ = false;
        showReport();
        setState(BpState::Idle);
    }
    void startValidation() {
        validatedText_ = editor_.getText();
        hasReport_ = false;
        showReport();
        bridge.requestBlueprintValidation(validatedText_);
        setState(BpState::Validating);
    }
    void confirmApply() {
        if (state_v_ != BpState::Ready) return;
        auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::WarningIcon, "Apply blueprint",
                                                                 "Apply this blueprint to the edit slot? The current circuit is replaced once the new one is ready. One undo step.",
                                                                 "Apply", "Cancel", this);
        juce::Component::SafePointer<BlueprintPanel> safe(this);
        juce::AlertWindow::showAsync(opts, [safe](int r) {
            if (safe == nullptr || r != 1) return;
            safe->setState(BpState::Applying);
            juce::MessageManager::callAsync([safe] {
                if (safe == nullptr) return;
                juce::String err;
                if (safe->bridge.applyBlueprint(safe->bridge.status().editSlot, safe->editor_.getText(), err)) {
                    safe->setState(BpState::Idle, "Applied.");
                } else {
                    safe->setState(BpState::Invalid, "Apply failed: " + err);
                }
            });
        });
    }
    void showReport() {
        bool ch = false;
        if (!hasReport_) {
            ch |= counts_->set("No validation report yet.", Tone::Dim);
            ch |= diag_->set("");
        } else {
            const auto cost = report_.estimatedFlopsPerSample > 0
                ? juce::String(report_.estimatedFlopsPerSample, 0) + " operations/electrical sample (estimate)"
                : report_.estimatedCostNsPerSample > 0
                    ? juce::String(report_.estimatedCostNsPerSample, 0) + " ns/sample (estimate)"
                    : juce::String("not available");
            ch |= counts_->set((report_.ok ? "Valid. " : "Invalid. ") + juce::String("Components ") + juce::String(report_.components) + ", nodes " + juce::String(report_.nodes) +
                                   ", nonlinear " + juce::String(report_.nonlinear) + ", unknowns " + juce::String(report_.unknowns) + ", estimated cost " +
                                   cost + "." + (report_.configuration.isEmpty() ? juce::String() : "\n" + report_.configuration),
                               report_.ok ? Tone::Ok : Tone::Danger);
            ch |= diag_->set(report_.diagnostics.isEmpty() ? juce::String(report_.ok ? "No diagnostics." : "No details reported.") : report_.diagnostics.joinIntoString("\n"),
                             report_.ok ? Tone::Dim : Tone::Danger);
        }
        if (ch) relayout();
    }
    void setState(BpState s, const juce::String& extra = {}) {
        state_v_ = s;
        static const char* const names[] = {"Idle", "Draft", "Validating", "Ready", "Invalid", "Applying"};
        static const Tone tones[] = {Tone::Dim, Tone::Warn, Tone::Normal, Tone::Ok, Tone::Danger, Tone::Normal};
        const int i = static_cast<int>(s);
        state_->set(juce::String("State: ") + names[i] + (extra.isEmpty() ? juce::String() : "  -  " + extra), tones[i]);
        validate_->setEnabled(s != BpState::Validating && s != BpState::Applying);
        apply_->setEnabled(s == BpState::Ready && bridge.supportsBlueprintApply());
        editor_.setReadOnly(s == BpState::Applying);
        relayout();
    }

    NoteLabel *state_ = nullptr, *counts_ = nullptr, *diag_ = nullptr;
    juce::TextEditor editor_;
    std::unique_ptr<juce::TextButton> validate_, reload_, apply_;
    BpState state_v_ = BpState::Idle;
    BlueprintReport report_;
    bool hasReport_ = false, loading_ = false;
    juce::String validatedText_;
    int slot_ = 0;
};

}  // namespace

std::unique_ptr<Panel> makeLifecyclePanel(EditorBridge& b) { return std::make_unique<LifecyclePanel>(b); }
std::unique_ptr<Panel> makeFaultsPanel(EditorBridge& b) { return std::make_unique<FaultsPanel>(b); }
std::unique_ptr<Panel> makeBlueprintPanel(EditorBridge& b) { return std::make_unique<BlueprintPanel>(b); }

}  // namespace cd::plugin::ui::pc
