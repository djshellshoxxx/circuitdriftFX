// CircuitDrift editor shell: vector control widgets (Knob, Switch, ChipButton).
// All sonic controls are bound with juce parameter attachments on bridge.parameters(); gestures
// are wrapped with bridge.beginGesture/endGesture so each interaction is one labelled undo step.
// Keyboard: arrows (1 %, Shift 0.1 %), Page Up/Down (10 %), Home/End, Enter/F2 numeric entry,
// Delete resets to default. Message thread only.
#pragma once
#include <memory>
#include "ui/Education.h"
#include "ui/shell/HelpOverlay.h"

namespace cd::plugin::ui::shell {

class Knob : public juce::Slider, private juce::Timer {
public:
    Knob(ShellContext& ctx, EditorBridge& bridge, const juce::String& paramId, const juce::String& label, Fmt fmt);
    ~Knob() override;

    bool isAttached() const noexcept { return attachment_ != nullptr; }
    const juce::String& paramId() const noexcept { return paramId_; }
    const juce::String& label() const noexcept { return label_; }
    juce::String valueText() const;
    void setDecorationScale(float s) noexcept { decoration_ = s; }

    // Numeric entry (Enter / F2 / context menu). commitText applies a typed value as one undo step.
    void showNumericEntry();
    bool isEntryOpen() const noexcept { return entry_ != nullptr; }
    bool commitText(const juce::String& text);
    void closeEntry();
    void resetToDefault();   // one undo step
    // Applies a normalized (0..1) change as one coalesced undo step (keyboard/wheel path).
    void nudgeNormalized(double delta, bool absolute);

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& k) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseEnter(const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover_ = false; repaint(); }
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { endKeyGesture(); repaint(); }

private:
    void timerCallback() override { endKeyGesture(); }
    void beginKeyGesture();
    void endKeyGesture();
    bool horizontal() const noexcept { return getWidth() > getHeight() * 1.7f; }
    void geometry(juce::Rectangle<float>& knob, juce::Rectangle<float>& label, juce::Rectangle<float>& value) const;

    ShellContext& ctx_;
    EditorBridge& bridge_;
    juce::String paramId_, label_;
    Fmt fmt_;
    std::unique_ptr<juce::SliderParameterAttachment> attachment_;
    std::unique_ptr<juce::TextEditor> entry_;
    bool hover_ = false, keyGesture_ = false, bipolar_ = false;
    float decoration_ = 1.0f;
};

// Toggle pill with LED, engraved label and an ON/OFF word (state never conveyed by colour alone).
class Switch : public juce::Button {
public:
    // paramId empty or missing in the tree => unattached (use setIndicator for read-only state).
    Switch(ShellContext& ctx, EditorBridge& bridge, const juce::String& paramId, const juce::String& label);
    ~Switch() override;
    bool isAttached() const noexcept { return attachment_ != nullptr; }
    const juce::String& paramId() const noexcept { return paramId_; }
    // Read-only indicator mode (e.g. host bypass in the full edition): clicks do nothing.
    void setIndicator(bool on, const juce::String& stateWordOn, const juce::String& stateWordOff);
    bool isIndicator() const noexcept { return indicator_; }
    void paintButton(juce::Graphics& g, bool hover, bool down) override;
    void clicked() override;
    void mouseUp(const juce::MouseEvent& e) override { userAction_ = true; juce::Button::mouseUp(e); userAction_ = false; }
    bool keyPressed(const juce::KeyPress& k) override { userAction_ = true; const bool r = juce::Button::keyPressed(k); userAction_ = false; return r; }
    bool keyStateChanged(bool down) override { userAction_ = true; const bool r = juce::Button::keyStateChanged(down); userAction_ = false; return r; }
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }

private:
    ShellContext& ctx_;
    EditorBridge& bridge_;
    juce::String paramId_, label_, wordOn_ = "ON", wordOff_ = "OFF";
    bool indicator_ = false, userAction_ = false, gestureOpen_ = false;
    std::unique_ptr<juce::ButtonParameterAttachment> attachment_;
};

enum class Glyph { None, Undo, Redo, Copy, Swap, Panels, Gear, Close };

// Segment / action chip with optional vector glyph.
class ChipButton : public juce::Button {
public:
    ChipButton(ShellContext& ctx, const juce::String& text, Glyph glyph = Glyph::None);
    void setText(const juce::String& t) { setButtonText(t); repaint(); }
    void setPrimary(bool p) { primary_ = p; repaint(); }
    void paintButton(juce::Graphics& g, bool hover, bool down) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    // Preferred width for the text at the chip's font.
    int preferredWidth() const;

private:
    ShellContext& ctx_;
    Glyph glyph_;
    bool primary_ = false;
};

void drawGlyph(juce::Graphics& g, Glyph glyph, juce::Rectangle<float> r, juce::Colour colour);

}  // namespace cd::plugin::ui::shell
