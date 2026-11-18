// CircuitDrift editor shell: tooltip / educational-mode overlay (CD-F08, 06-interface "Inspector
// data and education"). Opens after kTooltipDelayMs of hover or keyboard focus, closes on Escape,
// can be pinned (touch / reading). It is a child component of the editor root (no native window),
// so it scales with the editor and is testable headlessly. Message thread only.
#pragma once
#include "ui/shell/ShellCommon.h"

namespace cd::plugin::ui::shell {

// Associate help with a component: an education key (Education.cpp) or literal text.
void setHelpKey(juce::Component& c, const juce::String& educationKey);
void setTip(juce::Component& c, const juce::String& title, const juce::String& text);

struct HelpContent {
    bool valid = false;
    juce::String title, shortText, longText, audio, units, source;
};
HelpContent helpFor(juce::Component& c);

class HelpOverlay : public juce::Component, private juce::Timer, private juce::FocusChangeListener {
public:
    HelpOverlay(ShellContext& ctx, juce::Component& watched);
    ~HelpOverlay() override;

    // Opens immediately for `target` (the delay timer calls this). Returns false without help text.
    bool showFor(juce::Component& target);
    void dismiss();                 // Escape: also unpins
    bool isOpen() const noexcept { return open_; }
    bool isPinned() const noexcept { return pinned_; }
    void setPinned(bool p);
    juce::Component* target() const noexcept { return target_.getComponent(); }
    juce::Rectangle<int> bubbleBounds() const noexcept { return bubble_; }
    juce::String currentText() const { return text_; }
    // Re-renders after theme/educational-mode changes.
    void refresh();

    void paint(juce::Graphics& g) override;
    bool hitTest(int x, int y) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    // Receives events from every descendant of the watched root (Component is itself a
    // MouseListener, so a separate object avoids an ambiguous base).
    struct RootListener : juce::MouseListener {
        explicit RootListener(HelpOverlay& o) : owner(o) {}
        void mouseEnter(const juce::MouseEvent& e) override { owner.hoverChanged(e); }
        void mouseMove(const juce::MouseEvent& e) override { owner.hoverChanged(e); }
        void mouseExit(const juce::MouseEvent& e) override { owner.rootMouseExit(e); }
        void mouseDown(const juce::MouseEvent& e) override { owner.rootMouseDown(e); }
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { owner.rootWheel(e, w); }
        HelpOverlay& owner;
    };
    void rootMouseExit(const juce::MouseEvent& e);
    void rootMouseDown(const juce::MouseEvent& e);
    void rootWheel(const juce::MouseEvent&, const juce::MouseWheelDetails&);
    void globalFocusChanged(juce::Component* focused) override;
    void timerCallback() override;
    void hoverChanged(const juce::MouseEvent& e);
    static juce::Component* helpTargetFor(juce::Component* c);
    void layoutBubble();

    ShellContext& ctx_;
    juce::Component& watched_;
    RootListener listener_{*this};
    juce::Component::SafePointer<juce::Component> target_, pending_;
    bool open_ = false, pinned_ = false;
    juce::TextLayout layout_;
    juce::String text_;
    juce::Rectangle<int> bubble_, pinRect_;
};

}  // namespace cd::plugin::ui::shell
