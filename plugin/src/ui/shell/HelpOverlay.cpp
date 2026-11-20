#include "ui/shell/HelpOverlay.h"

#include "ui/Education.h"

namespace cd::plugin::ui::shell {

namespace {
const juce::Identifier kKey("cdHelpKey"), kTitle("cdHelpTitle"), kText("cdHelpText");
}

void setHelpKey(juce::Component& c, const juce::String& key) { c.getProperties().set(kKey, key); }
void setTip(juce::Component& c, const juce::String& title, const juce::String& text) {
    c.getProperties().set(kTitle, title);
    c.getProperties().set(kText, text);
}

HelpContent helpFor(juce::Component& c) {
    HelpContent h;
    if (c.getProperties().contains(kKey)) {
        if (const auto* e = findEducation(c.getProperties()[kKey].toString())) {
            h.valid = true;
            h.title = e->title;
            h.shortText = e->shortText;
            h.longText = e->longText;
            h.audio = e->audioEffect;
            h.units = e->units;
            h.source = e->source;
            return h;
        }
    }
    if (c.getProperties().contains(kText)) {
        h.valid = true;
        h.title = c.getProperties()[kTitle].toString();
        h.shortText = h.longText = c.getProperties()[kText].toString();
    }
    return h;
}

HelpOverlay::HelpOverlay(ShellContext& ctx, juce::Component& watched) : ctx_(ctx), watched_(watched) {
    setInterceptsMouseClicks(true, false);
    setAccessible(false);
    setWantsKeyboardFocus(false);
    watched_.addMouseListener(&listener_, true);
    juce::Desktop::getInstance().addFocusChangeListener(this);
}

HelpOverlay::~HelpOverlay() {
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    watched_.removeMouseListener(&listener_);
}

juce::Component* HelpOverlay::helpTargetFor(juce::Component* c) {
    for (int depth = 0; c != nullptr && depth < 4; ++depth, c = c->getParentComponent())
        if (c->getProperties().contains(kKey) || c->getProperties().contains(kText)) return c;
    return nullptr;
}

void HelpOverlay::hoverChanged(const juce::MouseEvent& e) {
    if (pinned_ || e.mods.isAnyMouseButtonDown()) return;
    auto* t = helpTargetFor(e.originalComponent);
    if (t == target_.getComponent() && open_) return;
    if (t == pending_.getComponent() && isTimerRunning()) return;
    dismiss();
    pending_ = t;
    if (t != nullptr) startTimer(kTooltipDelayMs);
}

void HelpOverlay::rootMouseExit(const juce::MouseEvent&) {
    if (pinned_) return;
    stopTimer();
    pending_ = nullptr;
    if (open_) dismiss();
}

void HelpOverlay::rootMouseDown(const juce::MouseEvent&) {
    if (pinned_) return;
    stopTimer();
    pending_ = nullptr;
    if (open_) dismiss();
}

void HelpOverlay::rootWheel(const juce::MouseEvent&, const juce::MouseWheelDetails&) {
    if (!pinned_) { stopTimer(); if (open_) dismiss(); }
}

void HelpOverlay::globalFocusChanged(juce::Component* focused) {
    if (pinned_) return;
    stopTimer();
    if (focused == nullptr || !watched_.isParentOf(focused)) {
        pending_ = nullptr;
        if (open_) dismiss();
        return;
    }
    // Focus produced by a mouse click is not a keyboard focus request.
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto sinceMouse = juce::Time::getCurrentTime() - src.getLastMouseDownTime();
    if (sinceMouse.inMilliseconds() >= 0 && sinceMouse.inMilliseconds() < 300) return;
    if (open_) dismiss();
    pending_ = helpTargetFor(focused);
    if (pending_ != nullptr) startTimer(kTooltipDelayMs);
}

void HelpOverlay::timerCallback() {
    stopTimer();
    if (auto* t = pending_.getComponent()) showFor(*t);
}

bool HelpOverlay::showFor(juce::Component& target) {
    auto* t = helpTargetFor(&target);
    if (t == nullptr) return false;
    const auto h = helpFor(*t);
    if (!h.valid) return false;
    target_ = t;
    const auto& th = ctx_.theme();
    const bool edu = ctx_.ui.educational;

    juce::AttributedString s;
    s.setLineSpacing(1.5f);
    s.append(h.title + "\n", uiFont(14.0f, true), th.text);
    s.append(edu ? h.longText : h.shortText, uiFont(12.5f), th.textDim);
    if (edu) {
        if (h.audio.isNotEmpty()) {
            s.append("\n\nWHAT CHANGES IN AUDIO\n", uiFont(11.0f, true), th.accent);
            s.append(h.audio, uiFont(12.5f), th.text);
        }
        if (h.units.isNotEmpty()) {
            s.append("\n\nUNITS  ", uiFont(11.0f, true), th.accent);
            s.append(h.units, uiFont(12.5f), th.text);
        }
        if (h.source.isNotEmpty()) {
            s.append("\n\nSOURCE  ", uiFont(11.0f, true), th.accent);
            s.append(h.source, uiFont(11.5f), th.textDim);
        }
    }
    text_ = h.title + "\n" + (edu ? h.longText : h.shortText);
    const float maxW = edu ? 400.0f : 290.0f;
    layout_.createLayout(s, maxW);
    open_ = true;
    layoutBubble();
    repaint();
    return true;
}

void HelpOverlay::layoutBubble() {
    auto* t = target_.getComponent();
    if (t == nullptr) return;
    const int pad = 10;
    const int w = juce::roundToInt(layout_.getWidth()) + pad * 2;
    const int h = juce::roundToInt(layout_.getHeight()) + pad * 2 + 16;  // footer line
    auto tb = getLocalArea(t, t->getLocalBounds());
    auto area = getLocalBounds().reduced(4);
    int x = juce::jlimit(area.getX(), std::max(area.getX(), area.getRight() - w), tb.getCentreX() - w / 2);
    int y = tb.getBottom() + 6;
    if (y + h > area.getBottom()) y = tb.getY() - 6 - h;
    y = juce::jlimit(area.getY(), std::max(area.getY(), area.getBottom() - h), y);
    bubble_ = {x, y, w, h};
    pinRect_ = {bubble_.getRight() - 58, bubble_.getBottom() - 20, 52, 16};
}

void HelpOverlay::dismiss() {
    stopTimer();
    pending_ = nullptr;
    const bool wasOpen = open_;
    open_ = false;
    pinned_ = false;
    target_ = nullptr;
    if (wasOpen) repaint();
}

void HelpOverlay::setPinned(bool p) {
    pinned_ = p && open_;
    repaint();
}

void HelpOverlay::refresh() {
    if (!open_) return;
    if (auto* t = target_.getComponent()) {
        const bool wasPinned = pinned_;
        showFor(*t);
        pinned_ = wasPinned;
    }
}

bool HelpOverlay::hitTest(int x, int y) { return open_ && pinRect_.contains(x, y); }

void HelpOverlay::mouseUp(const juce::MouseEvent& e) {
    if (open_ && pinRect_.contains(e.getPosition())) setPinned(!pinned_);
}

void HelpOverlay::paint(juce::Graphics& g) {
    if (!open_) return;
    const auto& th = ctx_.theme();
    auto b = bubble_.toFloat();
    juce::DropShadow(juce::Colours::black.withAlpha(th.dark ? 0.55f : 0.25f), 10, {0, 3})
        .drawForRectangle(g, bubble_);
    g.setColour(th.panelRaised);
    g.fillRoundedRectangle(b, th.corner);
    g.setColour(th.accent);
    g.drawRoundedRectangle(b.reduced(0.5f), th.corner, 1.2f);
    layout_.draw(g, b.reduced(10.0f, 10.0f).withHeight(b.getHeight() - 26.0f));
    // Footer: dismiss hint and pin toggle (text, not colour alone).
    g.setFont(uiFont(11.0f));
    g.setColour(th.textDim);
    g.drawText(pinned_ ? "Pinned. Esc closes" : "Esc closes", bubble_.getX() + 10, bubble_.getBottom() - 22, 150, 16,
               juce::Justification::centredLeft, false);
    auto pr = pinRect_.toFloat();
    g.setColour(pinned_ ? th.accent : th.inset);
    g.fillRoundedRectangle(pr, 3.0f);
    g.setColour(pinned_ ? th.textInverse : th.text);
    g.drawText(pinned_ ? "Unpin" : "Pin", pinRect_, juce::Justification::centred, false);
    g.setColour(th.border);
    g.drawRoundedRectangle(pr, 3.0f, 1.0f);
}

}  // namespace cd::plugin::ui::shell
