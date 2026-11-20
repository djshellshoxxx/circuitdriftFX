#include "ui/shell/ShellWidgets.h"

namespace cd::plugin::ui::shell {

namespace {
constexpr float kArcStart = -2.356f, kArcEnd = 2.356f;  // +/-135 degrees from 12 o'clock
juce::Point<float> polar(juce::Point<float> c, float r, float angle) {
    return {c.x + r * std::sin(angle), c.y - r * std::cos(angle)};
}
}  // namespace

// =============================================================================================
// Knob
// =============================================================================================
Knob::Knob(ShellContext& ctx, EditorBridge& bridge, const juce::String& paramId, const juce::String& label, Fmt fmt)
    : ctx_(ctx), bridge_(bridge), paramId_(paramId), label_(label), fmt_(fmt) {
    setSliderStyle(juce::Slider::RotaryVerticalDrag);
    setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    setScrollWheelEnabled(false);
    setPopupMenuEnabled(false);
    setMouseDragSensitivity(240);
    setWantsKeyboardFocus(true);
    setTitle(label);
    setHelpKey(*this, paramId);
    if (const auto* e = findEducation(paramId)) setDescription(e->shortText);
    if (auto* p = bridge.parameters().getParameter(paramId)) {
        attachment_ = std::make_unique<juce::SliderParameterAttachment>(*p, *this, nullptr);
        textFromValueFunction = [fmt](double v) { return formatValue(fmt, v); };
        valueFromTextFunction = [fmt, this](const juce::String& s) {
            if (s.trim() == formatValue(fmt, getValue())) return getValue();
            double v = 0;
            return parseValue(fmt, s, v) && v >= getMinimum() && v <= getMaximum() ? v : getValue();
        };
        bipolar_ = getMinimum() < 0.0 && getMaximum() > 0.0;
    }
    onDragStart = [this] { bridge_.beginGesture("Adjust " + label_); };
    onDragEnd = [this] { bridge_.endGesture(); };
}

Knob::~Knob() {
    stopTimer();
    endKeyGesture();
    attachment_.reset();
}

juce::String Knob::valueText() const { return formatValue(fmt_, getValue()); }

void Knob::geometry(juce::Rectangle<float>& knob, juce::Rectangle<float>& label, juce::Rectangle<float>& value) const {
    auto b = getLocalBounds().toFloat().reduced(2.0f);
    if (horizontal()) {
        const float d = std::min(b.getHeight(), b.getWidth() * 0.5f);
        knob = {b.getX(), b.getY() + (b.getHeight() - d) * 0.5f, d, d};
        auto text = b.withTrimmedLeft(d + 8.0f);
        label = text.removeFromTop(text.getHeight() * 0.5f);
        value = text;
    } else {
        const float lh = 13.0f, vh = 15.0f;
        const float d = std::max(10.0f, std::min(b.getWidth(), b.getHeight() - lh - vh - 2.0f));
        knob = {b.getCentreX() - d * 0.5f, b.getY(), d, d};
        value = {b.getX(), knob.getBottom() + 1.0f, b.getWidth(), vh};
        label = {b.getX(), value.getBottom(), b.getWidth(), lh};
    }
}

void Knob::resized() {
    if (entry_) {
        juce::Rectangle<float> k, l, v;
        geometry(k, l, v);
        entry_->setBounds(v.expanded(4.0f, 2.0f).toNearestInt().getIntersection(getLocalBounds()));
    }
}

void Knob::paint(juce::Graphics& g) {
    const auto& t = ctx_.theme();
    juce::Rectangle<float> kr, lr, vr;
    geometry(kr, lr, vr);
    const auto c = kr.getCentre();
    const float R = kr.getWidth() * 0.5f;
    const double prop = juce::jlimit(0.0, 1.0, valueToProportionOfLength(getValue()));
    const float angle = kArcStart + static_cast<float>(prop) * (kArcEnd - kArcStart);
    const bool focused = hasKeyboardFocus(false);

    // Scale ticks (11) and value arc.
    for (int i = 0; i <= 10; ++i) {
        const float a = kArcStart + (kArcEnd - kArcStart) * static_cast<float>(i) / 10.0f;
        g.setColour((i == 0 || i == 10 || i == 5) ? t.textDim : t.grid.brighter(t.dark ? 0.3f : -0.3f));
        g.drawLine({polar(c, R * 0.80f, a), polar(c, R * 0.90f, a)}, (i % 5 == 0) ? 1.4f : 1.0f);
    }
    juce::Path track;
    track.addCentredArc(c.x, c.y, R * 0.95f, R * 0.95f, 0.0f, kArcStart, kArcEnd, true);
    g.setColour(t.inset);
    g.strokePath(track, juce::PathStrokeType(std::max(2.0f, R * 0.12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::Path val;
    const float from = bipolar_ ? 0.0f : kArcStart;
    if (std::abs(angle - from) > 0.002f)
        val.addCentredArc(c.x, c.y, R * 0.95f, R * 0.95f, 0.0f, std::min(from, angle), std::max(from, angle), true);
    g.setColour(t.accent);
    g.strokePath(val, juce::PathStrokeType(std::max(2.0f, R * 0.12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Machined knob body.
    const float br = R * 0.66f;
    juce::Rectangle<float> body(c.x - br, c.y - br, br * 2.0f, br * 2.0f);
    g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.55f : 0.25f));
    g.fillEllipse(body.translated(0.0f, 1.5f).expanded(0.5f));
    g.setGradientFill(juce::ColourGradient(t.dark ? t.panelRaised.brighter(0.7f) : juce::Colours::white, c.x - br * 0.5f, c.y - br * 0.7f,
                                           t.dark ? t.inset : t.inset.darker(0.15f), c.x + br * 0.7f, c.y + br, true));
    g.fillEllipse(body);
    g.setColour(hover_ || focused ? t.accent : t.border);
    g.drawEllipse(body, hover_ || focused ? 1.6f : 1.1f);
    g.setColour(juce::Colours::white.withAlpha(t.dark ? 0.10f : 0.55f));
    g.drawEllipse(body.reduced(br * 0.28f), 0.8f);
    juce::Path ind;
    ind.startNewSubPath(polar(c, br * 0.22f, angle));
    ind.lineTo(polar(c, br * 0.94f, angle));
    g.setColour(t.accent);
    g.strokePath(ind, juce::PathStrokeType(std::max(2.0f, R * 0.11f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Labels.
    const bool isHorizontal = horizontal();
    const auto just = isHorizontal ? juce::Justification::centredLeft : juce::Justification::centred;
    drawEngraved(g, label_.toUpperCase(), lr, uiFont(10.5f, true).withExtraKerningFactor(0.06f), t, just, t.textDim);
    if (!entry_) {
        g.setFont(monoFont(12.0f, true));
        g.setColour(t.text);
        g.drawText(valueText(), vr, just, true);
    }
    if (focused) drawFocusRing(g, getLocalBounds().toFloat().reduced(0.5f), t, 5.0f);
}

void Knob::beginKeyGesture() {
    if (!keyGesture_) {
        bridge_.beginGesture("Adjust " + label_);
        keyGesture_ = true;
    }
    startTimer(500);
}

void Knob::endKeyGesture() {
    stopTimer();
    if (keyGesture_) {
        keyGesture_ = false;
        bridge_.endGesture();
    }
}

void Knob::nudgeNormalized(double delta, bool absolute) {
    if (!isAttached()) return;
    const double p = juce::jlimit(0.0, 1.0, absolute ? delta : valueToProportionOfLength(getValue()) + delta);
    beginKeyGesture();
    setValue(proportionOfLengthToValue(p), juce::sendNotificationSync);
}

bool Knob::keyPressed(const juce::KeyPress& k) {
    if (!isAttached() || !isEnabled()) return false;
    const auto mods = k.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown()) return false;
    const double fine = mods.isShiftDown() ? 0.1 : 1.0;
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey) { nudgeNormalized(0.01 * fine, false); return true; }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey) { nudgeNormalized(-0.01 * fine, false); return true; }
    if (code == juce::KeyPress::pageUpKey) { nudgeNormalized(0.10, false); return true; }
    if (code == juce::KeyPress::pageDownKey) { nudgeNormalized(-0.10, false); return true; }
    if (code == juce::KeyPress::homeKey) { nudgeNormalized(0.0, true); return true; }
    if (code == juce::KeyPress::endKey) { nudgeNormalized(1.0, true); return true; }
    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::F2Key) { showNumericEntry(); return true; }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
        beginKeyGesture();
        setValue(getDoubleClickReturnValue(), juce::sendNotificationSync);
        endKeyGesture();
        return true;
    }
    return false;
}

void Knob::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    if (e.mods.isPopupMenu()) {
        juce::PopupMenu m;
        m.addItem(1, "Enter value...");
        m.addItem(2, "Reset to default");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                        [safe = juce::Component::SafePointer<Knob>(this)](int r) {
                            if (safe == nullptr) return;
                            if (r == 1) safe->showNumericEntry();
                            if (r == 2) safe->resetToDefault();
                        });
        return;
    }
    juce::Slider::mouseDown(e);
}

void Knob::mouseDoubleClick(const juce::MouseEvent&) { resetToDefault(); }

void Knob::resetToDefault() {
    if (!isAttached()) return;
    bridge_.beginGesture("Reset " + label_);
    setValue(getDoubleClickReturnValue(), juce::sendNotificationSync);
    bridge_.endGesture();
}

void Knob::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    const float d = w.deltaY != 0.0f ? w.deltaY : -w.deltaX;
    if (d == 0.0f || !isAttached()) return;
    nudgeNormalized((d > 0 ? 1.0 : -1.0) * (e.mods.isShiftDown() ? 0.002 : 0.01), false);
}

void Knob::showNumericEntry() {
    if (entry_ || !isAttached()) return;
    const auto& t = ctx_.theme();
    entry_ = std::make_unique<juce::TextEditor>(label_ + " value");
    entry_->setFont(monoFont(12.0f, true));
    entry_->setJustification(juce::Justification::centred);
    entry_->setColour(juce::TextEditor::backgroundColourId, t.inset);
    entry_->setColour(juce::TextEditor::textColourId, t.text);
    entry_->setColour(juce::TextEditor::outlineColourId, t.border);
    entry_->setColour(juce::TextEditor::focusedOutlineColourId, t.focus);
    entry_->setColour(juce::TextEditor::highlightColourId, t.accent);
    entry_->setColour(juce::TextEditor::highlightedTextColourId, t.textInverse);
    entry_->setColour(juce::CaretComponent::caretColourId, t.accent);
    entry_->setText(valueText(), false);
    entry_->setTitle(label_ + " value, type a number and press Enter");
    entry_->onReturnKey = [this] { commitText(entry_->getText()); closeEntry(); grabKeyboardFocus(); };
    entry_->onEscapeKey = [this] { closeEntry(); grabKeyboardFocus(); };
    entry_->onFocusLost = [this] { closeEntry(); };
    addAndMakeVisible(*entry_);
    resized();
    entry_->grabKeyboardFocus();
    entry_->selectAll();
    repaint();
}

void Knob::closeEntry() {
    if (!entry_) return;
    auto* e = entry_.release();
    e->onFocusLost = nullptr;
    e->onReturnKey = nullptr;
    e->onEscapeKey = nullptr;
    removeChildComponent(e);
    juce::MessageManager::callAsync([e] { delete e; });
    repaint();
}

bool Knob::commitText(const juce::String& text) {
    if (!isAttached()) return false;
    if (text.trim() == valueText()) return true;
    double v = 0;
    if (!parseValue(fmt_, text, v) || v < getMinimum() || v > getMaximum()) return false;
    bridge_.beginGesture("Set " + label_);
    setValue(v, juce::sendNotificationSync);
    bridge_.endGesture();
    return true;
}

// =============================================================================================
// Switch
// =============================================================================================
Switch::Switch(ShellContext& ctx, EditorBridge& bridge, const juce::String& paramId, const juce::String& label)
    : juce::Button(label), ctx_(ctx), bridge_(bridge), paramId_(paramId), label_(label) {
    setWantsKeyboardFocus(true);
    setTitle(label);
    if (paramId.isNotEmpty()) {
        setHelpKey(*this, paramId);
        if (const auto* e = findEducation(paramId)) setDescription(e->shortText);
        if (auto* p = bridge.parameters().getParameter(paramId)) {
            setClickingTogglesState(true);
            attachment_ = std::make_unique<juce::ButtonParameterAttachment>(*p, *this, nullptr);
        }
    }
    onClick = [this] {
        if (gestureOpen_) { gestureOpen_ = false; bridge_.endGesture(); }
    };
}

Switch::~Switch() { attachment_.reset(); }

void Switch::clicked() {
    // Only user-initiated clicks open an undo transaction (parameter updates from elsewhere
    // re-sync the toggle through the same path and must not create history entries).
    if (attachment_ && !indicator_ && userAction_ && !gestureOpen_) {
        gestureOpen_ = true;
        bridge_.beginGesture("Toggle " + label_);
    }
}

void Switch::setIndicator(bool on, const juce::String& wordOn, const juce::String& wordOff) {
    indicator_ = true;
    setClickingTogglesState(false);
    wordOn_ = wordOn;
    wordOff_ = wordOff;
    if (getToggleState() != on) setToggleState(on, juce::dontSendNotification);
    repaint();
}

void Switch::paintButton(juce::Graphics& g, bool hover, bool down) {
    const auto& t = ctx_.theme();
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    const bool on = getToggleState();
    drawPlate(g, r, t, !down, 5.0f);
    if (hover) {
        g.setColour(t.accent.withAlpha(0.8f));
        g.drawRoundedRectangle(r, 5.0f, 1.2f);
    }
    // LED
    const float led = std::min(9.0f, r.getHeight() * 0.38f);
    juce::Rectangle<float> lr(r.getX() + 9.0f, r.getCentreY() - led * 0.5f, led, led);
    if (on) {
        g.setColour(t.accent.withAlpha(0.28f));
        g.fillEllipse(lr.expanded(3.0f));
        g.setColour(t.accent);
        g.fillEllipse(lr);
    } else {
        g.setColour(t.inset);
        g.fillEllipse(lr);
    }
    g.setColour(on ? t.accent : t.textDim);
    g.drawEllipse(lr, 1.1f);
    const float wordW = textWidth(monoFont(10.5f, true), on ? wordOn_ : wordOff_) + 4.0f;
    auto text = r.withTrimmedLeft(lr.getRight() + 6.0f - r.getX()).withTrimmedRight(wordW + 8.0f);
    drawEngraved(g, label_.toUpperCase(), text, uiFont(10.5f, true).withExtraKerningFactor(0.05f), t, juce::Justification::centredLeft, t.text);
    g.setFont(monoFont(10.5f, true));
    g.setColour(on ? t.accent : t.textDim);
    g.drawText(on ? wordOn_ : wordOff_, r.withTrimmedRight(7.0f), juce::Justification::centredRight, false);
    if (hasKeyboardFocus(false)) drawFocusRing(g, r.expanded(1.0f), t, 6.0f);
}

// =============================================================================================
// ChipButton + glyphs
// =============================================================================================
void drawGlyph(juce::Graphics& g, Glyph glyph, juce::Rectangle<float> r, juce::Colour colour) {
    const auto c = r.getCentre();
    const float s = std::min(r.getWidth(), r.getHeight());
    const float k = s * 0.5f;
    g.setColour(colour);
    const juce::PathStrokeType stroke(std::max(1.4f, s * 0.11f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    auto head = [&](juce::Point<float> tip, juce::Point<float> dir, float len, float w) {
        juce::Point<float> perp(-dir.y, dir.x);
        juce::Path h;
        h.addTriangle(tip, tip - dir * len + perp * w, tip - dir * len - perp * w);
        g.fillPath(h);
    };
    switch (glyph) {
        case Glyph::Undo:
        case Glyph::Redo: {
            const float rad = k * 0.72f;
            const float a0 = -0.55f, a1 = 3.9f;
            juce::Path p;
            p.addCentredArc(0.0f, k * 0.12f, rad, rad, 0.0f, a0, a1, true);
            const juce::AffineTransform tf = glyph == Glyph::Redo ? juce::AffineTransform::scale(-1.0f, 1.0f).translated(c.x, c.y)
                                                                    : juce::AffineTransform::translation(c.x, c.y);
            g.strokePath(p, stroke, tf);
            auto tip = polar({0.0f, k * 0.12f}, rad, a0);
            juce::Point<float> d(-std::cos(a0), -std::sin(a0));
            juce::Path h;
            const float hl = k * 0.62f, hw = k * 0.36f;
            juce::Point<float> perp(-d.y, d.x);
            h.addTriangle(tip + d * hl * 0.5f, tip - d * hl * 0.5f + perp * hw, tip - d * hl * 0.5f - perp * hw);
            g.fillPath(h, tf);
            break;
        }
        case Glyph::Copy: {
            auto a = juce::Rectangle<float>(c.x - k * 0.75f, c.y - k * 0.55f, k, k * 1.1f);
            auto b = a.translated(k * 0.6f, k * 0.0f);
            g.drawRoundedRectangle(a, 1.5f, stroke.getStrokeThickness());
            g.fillRoundedRectangle(b.reduced(0.0f), 1.5f);
            break;
        }
        case Glyph::Swap: {
            juce::Path p;
            p.startNewSubPath(c.x - k * 0.8f, c.y - k * 0.32f); p.lineTo(c.x + k * 0.55f, c.y - k * 0.32f);
            p.startNewSubPath(c.x + k * 0.8f, c.y + k * 0.32f); p.lineTo(c.x - k * 0.55f, c.y + k * 0.32f);
            g.strokePath(p, stroke);
            head({c.x + k * 0.85f, c.y - k * 0.32f}, {1, 0}, k * 0.5f, k * 0.3f);
            head({c.x - k * 0.85f, c.y + k * 0.32f}, {-1, 0}, k * 0.5f, k * 0.3f);
            break;
        }
        case Glyph::Panels: {
            auto box = juce::Rectangle<float>(c.x - k * 0.85f, c.y - k * 0.65f, k * 1.7f, k * 1.3f);
            g.drawRoundedRectangle(box, 2.0f, stroke.getStrokeThickness());
            g.fillRect(box.withLeft(box.getX() + box.getWidth() * 0.62f).reduced(0.0f, 0.5f));
            break;
        }
        case Glyph::Gear: {
            juce::Path p;
            const float ro = k * 0.92f, rb = k * 0.68f, ri = k * 0.28f;
            for (int i = 0; i < 8; ++i) {
                juce::Path tooth;
                tooth.addRectangle(-k * 0.2f, -ro, k * 0.4f, ro - rb * 0.6f);
                p.addPath(tooth, juce::AffineTransform::rotation(static_cast<float>(i) * juce::MathConstants<float>::pi / 4.0f));
            }
            p.addEllipse(-rb, -rb, rb * 2, rb * 2);
            p.addEllipse(-ri, -ri, ri * 2, ri * 2);
            p.setUsingNonZeroWinding(false);
            g.fillPath(p, juce::AffineTransform::translation(c.x, c.y));
            break;
        }
        case Glyph::Close: {
            juce::Path p;
            p.startNewSubPath(c.x - k * 0.5f, c.y - k * 0.5f); p.lineTo(c.x + k * 0.5f, c.y + k * 0.5f);
            p.startNewSubPath(c.x + k * 0.5f, c.y - k * 0.5f); p.lineTo(c.x - k * 0.5f, c.y + k * 0.5f);
            g.strokePath(p, stroke);
            break;
        }
        case Glyph::None: break;
    }
}

ChipButton::ChipButton(ShellContext& ctx, const juce::String& text, Glyph glyph)
    : juce::Button(text), ctx_(ctx), glyph_(glyph) {
    setWantsKeyboardFocus(true);
    setTitle(text);
}

int ChipButton::preferredWidth() const {
    const float tw = getButtonText().isEmpty() ? 0.0f : textWidth(uiFont(12.5f, true), getButtonText());
    const float gw = glyph_ == Glyph::None ? 0.0f : 18.0f + (tw > 0 ? 4.0f : 0.0f);
    return juce::roundToInt(tw + gw + 20.0f);
}

void ChipButton::paintButton(juce::Graphics& g, bool hover, bool down) {
    const auto& t = ctx_.theme();
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    const bool on = getToggleState() || primary_;
    const bool enabled = isEnabled();
    juce::Colour fill = on ? t.accent : t.panelRaised;
    if (!on && (hover || down)) fill = fill.interpolatedWith(t.accent, down ? 0.30f : 0.15f);
    if (on && down) fill = fill.darker(0.15f);
    g.setGradientFill(juce::ColourGradient(fill.brighter(on ? 0.0f : (t.dark ? 0.12f : 0.0f)), r.getX(), r.getY(),
                                           fill.darker(on ? 0.10f : (t.dark ? 0.15f : 0.04f)), r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(on ? t.accent.darker(0.25f) : t.border);
    g.drawRoundedRectangle(r, 5.0f, 1.2f);
    const auto textColour = (on ? t.textInverse : t.text).withAlpha(enabled ? 1.0f : 0.45f);
    auto inner = r.reduced(6.0f, 2.0f);
    if (glyph_ != Glyph::None) {
        const bool hasText = getButtonText().isNotEmpty();
        auto gr = hasText ? inner.removeFromLeft(16.0f) : inner;
        drawGlyph(g, glyph_, gr.reduced(hasText ? 0.0f : 2.0f), textColour);
        if (hasText) inner.removeFromLeft(3.0f);
    }
    if (getButtonText().isNotEmpty()) {
        g.setFont(uiFont(12.5f, true));
        g.setColour(textColour);
        g.drawText(getButtonText(), inner, juce::Justification::centred, true);
    }
    if (hasKeyboardFocus(false)) drawFocusRing(g, r.expanded(1.0f), t, 6.0f);
}

}  // namespace cd::plugin::ui::shell
