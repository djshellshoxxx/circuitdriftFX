#include "ui/workbench/InspectorPanel.h"

#include "ui/Education.h"

namespace cd::plugin::ui {
namespace {
using juce::Graphics;
using juce::Rectangle;

juce::Font fnt(float h, bool bold = false) {
    juce::Font f{juce::FontOptions(h)};
    return bold ? f.boldened() : f;
}

const char* roleName(engine::ComponentRole r) {
    static const char* n[] = {"Input coupling", "Grid leak / bias network", "Active element", "Plate load", "Cathode resistor", "Cathode bypass",
                              "Output coupling", "Output transformer", "Decoupling", "Decoupling resistor", "Screen resistor", "Screen bypass",
                              "Reservoir", "Rectifier", "Filter resistor", "Filter capacitor", "Chassis thermal node"};
    const int i = static_cast<int>(r);
    return i >= 0 && i < 17 ? n[i] : "?";
}

float measureWrapped(const juce::String& text, const juce::Font& f, float width, Graphics* g, Rectangle<float> at, juce::Colour c) {
    juce::AttributedString as;
    as.setJustification(juce::Justification::topLeft);
    as.append(text, f, c);
    juce::TextLayout tl;
    tl.createLayout(as, width);
    if (g) tl.draw(*g, at.withWidth(width));
    return tl.getHeight();
}
}  // namespace

InspectorPanel::InspectorPanel(EditorBridge& bridge, PcbCanvas& canvas) : bridge_(bridge), canvas_(canvas), ops_(bridge) {
    setTitle("Component inspector");
    field_.setMultiLine(false);
    field_.setReturnKeyStartsNewLine(false);
    field_.setSelectAllWhenFocused(true);
    field_.setTitle("Numeric value");
    field_.setTooltip("Type a value with optional unit (for example 470u, 0.47 mF). Enter applies; invalid text changes nothing.");
    field_.onReturnKey = [this] { applyNumeric(field_.getText()); syncField(true); relayout(); repaint(); };
    field_.onEscapeKey = [this] { syncField(true); field_.giveAwayKeyboardFocus(); };
    addChildComponent(field_);
    resetButton_.setTitle("Reset to factory base value");
    resetButton_.onClick = [this] { canvas_.resetSelected(); refresh(); };
    addChildComponent(resetButton_);
    static const char* names[3] = {"Carbon Comp", "Metal Film", "Wirewound"};
    for (int i = 0; i < 3; ++i) {
        material_[i].setButtonText(names[i]);
        material_[i].setTitle(juce::String("Resistor material: ") + names[i]);
        material_[i].setRadioGroupId(7201);
        material_[i].setClickingTogglesState(true);
        material_[i].onClick = [this, i] { canvas_.setMaterial(i); refresh(); };
        addChildComponent(material_[i]);
    }
    setTheme(darkTheme());
}

void InspectorPanel::setTheme(const Theme& t) {
    theme_ = &t;
    field_.setColour(juce::TextEditor::backgroundColourId, t.inset);
    field_.setColour(juce::TextEditor::textColourId, t.text);
    field_.setColour(juce::TextEditor::outlineColourId, t.border);
    field_.setColour(juce::TextEditor::focusedOutlineColourId, t.focus);
    field_.setColour(juce::CaretComponent::caretColourId, t.text);
    field_.applyFontToAllText(fnt(13.0f));
    for (auto* b : {&resetButton_, &material_[0], &material_[1], &material_[2]}) {
        b->setColour(juce::TextButton::buttonColourId, t.panelRaised);
        b->setColour(juce::TextButton::buttonOnColourId, t.accent);
        b->setColour(juce::TextButton::textColourOffId, t.text);
        b->setColour(juce::TextButton::textColourOnId, t.textInverse);
    }
    repaint();
}

juce::String InspectorPanel::fieldUnitLabel() const {
    switch (canvas_.selected()) {
        case 2: return "Bias [normalized -1..1]";
        case 12: return juce::String("Reservoir capacity [") + juce::String(juce::CharPointer_UTF8("\xC2\xB5")) + "F]";
        default: return "Age [years]";
    }
}

void InspectorPanel::syncField(bool force) {
    const int id = canvas_.selected();
    if (id < 0 || !editParamFor(id)) return;
    if (!force && field_.hasKeyboardFocus(true)) return;
    juce::String t;
    if (id == 12) t = juce::String(ops_.real("supply.railCapacity"), 1);
    else if (id == 2) t = juce::String(ops_.real("stage.bias"), 2);
    else t = juce::String(ops_.real("condition.ageYears"), 1);
    if (field_.getText() != t) field_.setText(t, false);
}

bool InspectorPanel::applyNumeric(const juce::String& text) {
    error_.clear();
    const int id = canvas_.selected();
    const char* base = id >= 0 ? editParamFor(id) : nullptr;
    if (!base) { error_ = "Nothing editable selected"; return false; }
    auto* p = ops_.param(base);
    if (!p) { error_ = "Parameter unavailable"; return false; }
    const auto range = p->getNormalisableRange().getRange();
    double value = 0;
    if (id == 12) {
        const auto q = gesture::parseQuantity(text.toStdString(), "F", 1e-6);
        if (!q.ok) { error_ = q.error; return false; }
        value = q.value * 1e6;  // uF
    } else {
        const auto q = gesture::parseQuantity(text.toStdString(), "", 1.0);
        if (!q.ok) { error_ = q.error; return false; }
        value = q.value;
    }
    if (value < range.getStart() - 1e-9 || value > range.getEnd() + 1e-9) {
        error_ = "Out of range: " + juce::String(range.getStart(), 2) + " to " + juce::String(range.getEnd(), 2);
        return false;
    }
    ops_.begin(juce::String("Set ") + pcb::parts()[static_cast<std::size_t>(id)].key + " (numeric)", {base});
    ops_.setReal(base, value);
    ops_.end();
    canvas_.refreshForEdit();
    return true;
}

void InspectorPanel::refresh() {
    if (fieldFor_ != canvas_.selected()) { fieldFor_ = canvas_.selected(); error_.clear(); syncField(true); }
    else syncField(false);
    relayout();
    repaint();
}

void InspectorPanel::relayout() {
    contentH_ = layoutPass(nullptr);
    scrollY_ = std::clamp(scrollY_, 0, std::max(0, contentH_ - getHeight()));
}

void InspectorPanel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    scrollY_ = std::clamp(scrollY_ - static_cast<int>(w.deltaY * 120.0f), 0, std::max(0, contentH_ - getHeight()));
    relayout();
    repaint();
}

void InspectorPanel::paint(Graphics& g) {
    const auto& t = *theme_;
    g.setColour(t.panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), t.corner);
    g.setColour(t.border.withAlpha(0.7f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), t.corner, 1.0f);
    g.saveState();
    g.reduceClipRegion(getLocalBounds().reduced(1));
    drawn_.clear();
    contentH_ = layoutPass(&g);
    g.restoreState();
    if (contentH_ > getHeight()) {  // scroll indicator
        const float th = static_cast<float>(getHeight()) * static_cast<float>(getHeight()) / static_cast<float>(contentH_);
        const float ty = static_cast<float>(scrollY_) / static_cast<float>(contentH_ - getHeight()) * (static_cast<float>(getHeight()) - th);
        g.setColour(t.border.withAlpha(0.8f));
        g.fillRoundedRectangle(static_cast<float>(getWidth()) - 5.0f, ty + 1.0f, 3.0f, th - 2.0f, 1.5f);
    }
}

int InspectorPanel::layoutPass(Graphics* g) {
    const auto& t = *theme_;
    const int id = canvas_.selected();
    const float pad = 10.0f;
    const float w = static_cast<float>(getWidth()) - 2 * pad - 4.0f;
    float y = pad - static_cast<float>(scrollY_);
    const float y0 = y;
    auto note = [&](const juce::String& s) { if (g) drawn_.add(s); };
    auto heading = [&](const juce::String& s) {
        y += 6.0f;
        if (g) {
            g->setColour(t.accent);
            g->setFont(fnt(10.5f, true));
            g->drawText(s.toUpperCase(), Rectangle<float>(pad, y, w, 13.0f), juce::Justification::centredLeft);
            g->setColour(t.border.withAlpha(0.5f));
            g->drawLine(pad, y + 14.0f, pad + w, y + 14.0f, 0.7f);
        }
        note(s);
        y += 18.0f;
    };
    auto kv = [&](const juce::String& k, const juce::String& v, juce::Colour vc) {
        const float kw = std::min(98.0f, w * 0.38f);
        const float hv = measureWrapped(v, fnt(12.0f), w - kw - 4.0f, g, Rectangle<float>(pad + kw + 4.0f, y, w - kw - 4.0f, 200.0f), vc);
        if (g) {
            g->setColour(t.textDim);
            g->setFont(fnt(11.0f));
            g->drawText(k, Rectangle<float>(pad, y, kw, 15.0f), juce::Justification::topLeft, true);
        }
        note(k + ": " + v);
        y += std::max(hv, 15.0f) + 3.0f;
    };
    auto para = [&](const juce::String& s, float size, juce::Colour c) {
        const float h = measureWrapped(s, fnt(size), w, g, Rectangle<float>(pad, y, w, 400.0f), c);
        note(s);
        y += h + 4.0f;
    };

    const bool showField = id >= 0 && editParamFor(id) != nullptr;
    const bool isRes = id >= 0 && pcb::parts()[static_cast<std::size_t>(id)].kind == pcb::Kind::Resistor;
    field_.setVisible(false); resetButton_.setVisible(false);
    for (auto& m : material_) m.setVisible(false);

    if (id < 0) {
        if (g) { g->setColour(t.text); g->setFont(fnt(15.0f, true)); g->drawText("Inspector", Rectangle<float>(pad, y, w, 20.0f), juce::Justification::centredLeft); }
        note("Inspector");
        y += 26.0f;
        para("Select a component on the board (click it, or press [ and ] ) to see its identity, values, live telemetry and condition.", 12.0f, t.textDim);
        heading("Direct manipulation");
        para("Tube: drag up/down = bias (200 px = full range, Shift = fine).", 11.5f, t.text);
        para("Reservoir capacitor / transformer: drag sideways, scroll or pinch = supply capacity (ratio sqrt2 doubles C).", 11.5f, t.text);
        para("Resistor: Age brush drags change slot age (Alt reverses); the Material tool picks Carbon Comp / Metal Film / Wirewound.", 11.5f, t.text);
        para("Arrows adjust +-1 % (Shift +-0.1 %), Home resets, Escape cancels a gesture. Every gesture is one undo step.", 11.5f, t.text);
        heading("Provenance");
        para("All values come from a synthetic fixture (G01 open): round illustrative numbers, not calibrated hardware.", 11.5f, t.textDim);
        return static_cast<int>(y - y0 + pad);
    }

    const auto& part = pcb::parts()[static_cast<std::size_t>(id)];
    const auto* info = engine::findComponent(part.key);
    const auto& r = canvas_.readout(id);
    const bool stale = canvas_.stale();
    if (g) {
        g->setColour(t.text);
        g->setFont(fnt(15.0f, true));
        g->drawText(info ? info->displayName : part.key, Rectangle<float>(pad, y, w, 20.0f), juce::Justification::centredLeft, true);
    }
    note(info ? info->displayName : part.key);
    y += 24.0f;
    if (stale) {
        if (g) {
            g->setColour(t.warning);
            g->fillRoundedRectangle(Rectangle<float>(pad, y, 120.0f, 15.0f), 3.0f);
            g->setColour(t.textInverse);
            g->setFont(fnt(10.5f, true));
            g->drawText("! STALE TELEMETRY", Rectangle<float>(pad, y, 120.0f, 15.0f), juce::Justification::centred);
        }
        note("STALE TELEMETRY");
        y += 19.0f;
    }

    heading("Identity");
    kv("Key", part.key, t.text);
    if (info) {
        kv("Role", roleName(info->role), t.text);
        kv("Scope", info->scope == engine::ComponentScope::PerChannel ? "per channel (shown: ch " + juce::String(canvas_.channel() + 1) + ")" : "per slot (shared by channels)", t.text);
        kv("Material", info->material, t.text);
        kv("Model", info->modelId, t.text);
        kv("Primary unit", info->units, t.text);
        kv("Provenance", info->provenance, t.textDim);
    }

    heading("Values (units)");
    if (r.values.empty()) para("No settable values for this component.", 11.5f, t.textDim);
    for (const auto& row : r.values) kv(row.label, row.value, t.text);

    heading(stale ? "Live (last known, stale)" : "Live");
    if (r.live.empty()) para("No live telemetry yet.", 11.5f, t.textDim);
    for (const auto& row : r.live) kv(row.label, row.value, stale ? t.textDim : t.text);
    if (!r.hasVoltage && !r.voltageNote.isEmpty()) kv("Voltage", r.voltageNote, t.textDim);

    heading("Condition");
    kv("State", r.condition, r.fault ? t.danger : t.text);
    if (r.fault && g) { /* fault text already carries the type; badge is on the board */ }

    if (showField || isRes) {
        heading("Edit");
        if (showField) {
            if (g) { g->setColour(t.textDim); g->setFont(fnt(11.0f)); g->drawText(fieldUnitLabel(), Rectangle<float>(pad, y, w, 14.0f), juce::Justification::centredLeft); }
            note(fieldUnitLabel());
            y += 16.0f;
            field_.setBounds(juce::Rectangle<float>(pad, y, w - 60.0f, 24.0f).toNearestInt());
            resetButton_.setBounds(juce::Rectangle<float>(pad + w - 54.0f, y, 54.0f, 24.0f).toNearestInt());
            field_.setVisible(true); resetButton_.setVisible(true);
            y += 28.0f;
            if (error_.isNotEmpty()) {
                if (g) { g->setColour(t.danger); g->setFont(fnt(11.0f, true)); g->drawText("! " + error_, Rectangle<float>(pad, y, w, 14.0f), juce::Justification::centredLeft); }
                note(error_);
                y += 17.0f;
            }
        }
        if (isRes) {
            if (g) { g->setColour(t.textDim); g->setFont(fnt(11.0f)); g->drawText("Material (separate from the Age brush)", Rectangle<float>(pad, y, w, 14.0f), juce::Justification::centredLeft); }
            note("Material (separate from the Age brush)");
            y += 16.0f;
            const int cur = static_cast<int>(std::lround(ops_.real("condition.resistorType")));
            const float bw = (w - 8.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                material_[i].setBounds(juce::Rectangle<float>(pad + i * (bw + 4.0f), y, bw, 24.0f).toNearestInt());
                material_[i].setToggleState(i == cur, juce::dontSendNotification);
                material_[i].setVisible(true);
            }
            y += 28.0f;
        }
    }

    heading("About");
    const auto* edu = findEducation(part.key);
    if (edu) {
        para(edu->title, 12.5f, t.text);
        para(educational_ && juce::String(edu->longText).isNotEmpty() ? edu->longText : edu->shortText, 11.5f, t.text);
        if (juce::String(edu->audioEffect).isNotEmpty()) para(juce::String("What changes in audio: ") + edu->audioEffect, 11.5f, t.text);
        para(juce::String("Source: ") + edu->source, 10.5f, t.textDim);
    } else {
        para("No bundled education text for this component yet.", 11.5f, t.textDim);
        para(juce::String("Provenance: ") + (info ? info->provenance : "synthetic fixture (G01)"), 10.5f, t.textDim);
    }
    return static_cast<int>(y - y0 + pad);
}

}  // namespace cd::plugin::ui
