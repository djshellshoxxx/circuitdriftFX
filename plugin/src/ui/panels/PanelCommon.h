// CircuitDrift plugin: shared building blocks of the secondary panels (panels owner). Message thread
// only. Every panel is a scrollable flow of items packed left-to-right and wrapped, so nothing can
// overlap or clip at narrow widths. Widgets carry accessible titles and tooltips.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ui/Theme.h"
#include "ui/panels/Panels.h"

namespace cd::plugin::ui::pc {

void applyTheme(juce::LookAndFeel_V4& laf, const Theme& t);

enum class Tone { Normal, Dim, Warn, Danger, Ok, Accent };

class FlowContent;

// Wrapped static text (notes, status lines). Height follows the width it is given.
class NoteLabel : public juce::Component {
public:
    explicit NoteLabel(Tone tone = Tone::Dim, float size = 0.0f, bool bold = false) : tone_(tone), size_(size), bold_(bold) {
        setInterceptsMouseClicks(false, false);
    }
    // Returns true when the visible text or tone changed (the owner should relayout).
    bool set(const juce::String& text, Tone tone);
    bool set(const juce::String& text) { return set(text, tone_); }
    const juce::String& text() const noexcept { return text_; }
    int heightFor(int width) const;
    void paint(juce::Graphics& g) override;

private:
    juce::AttributedString build(int width) const;
    juce::String text_;
    Tone tone_;
    float size_;
    bool bold_;
};

// Bound to a registry parameter through juce::ParameterAttachment; edits are wrapped in bridge
// gestures so each user action is one undo step. Missing parameters disable the control.
class ParamBinding {
public:
    ParamBinding(EditorBridge& b, juce::String undoLabel) : bridge_(b), label_(std::move(undoLabel)) {}
    ~ParamBinding() { unbind(); }
    void bind(const juce::String& paramId, std::function<void(float)> onParamChanged);
    juce::RangedAudioParameter* lookup(const juce::String& id) const { return bridge_.parameters().getParameter(id); }
    void unbind() { endPart(); attach_.reset(); param_ = nullptr; id_ = {}; }
    bool bound() const noexcept { return param_ != nullptr; }
    juce::RangedAudioParameter* parameter() const noexcept { return param_; }
    const juce::String& id() const noexcept { return id_; }
    void setComplete(float denormalised);   // begin + set + end, one undo step
    void beginPart();
    void setPart(float denormalised);
    void endPart();
    float value() const;

private:
    EditorBridge& bridge_;
    juce::String label_;
    juce::String id_;
    juce::RangedAudioParameter* param_ = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attach_;
    bool inPart_ = false;
};

class ParamSlider : public juce::Component {
public:
    ParamSlider(EditorBridge& b, const juce::String& name, const juce::String& tooltip);
    ~ParamSlider() override;
    void bind(const juce::String& paramId);
    void resized() override;
    using PlainParser = std::function<std::optional<double>(const std::string&)>;
    void setDisplay(std::function<juce::String(double)> fmt, PlainParser parser = {});
    juce::Slider slider;
    juce::Label label;
    ParamBinding binding;
    static constexpr int kHeight = 46;

private:
    bool inPart_ = false, updating_ = false;
    std::function<juce::String(double)> fmt_;
    PlainParser parse_;
};

class ParamChoice : public juce::Component {
public:
    ParamChoice(EditorBridge& b, const juce::String& name, const juce::String& tooltip);
    // Optional human labels (same order as the parameter's choices); default uses the choice names.
    void bind(const juce::String& paramId, const juce::StringArray& labels = {});
    void resized() override;
    juce::ComboBox box;
    juce::Label label;
    ParamBinding binding;
    int index() const { return box.getSelectedId() - 1; }
    static constexpr int kHeight = 48;

private:
    bool updating_ = false;
};

class ParamToggle : public juce::ToggleButton {
public:
    ParamToggle(EditorBridge& b, const juce::String& text, const juce::String& tooltip);
    void bind(const juce::String& paramId);
    ParamBinding binding;
    static constexpr int kHeight = 28;
    std::function<void(bool)> onUserChange;

private:
    bool updating_ = false;
};

int textWidth(const juce::String& text, float height, bool bold = false);
// Width a button needs for its caption.
int buttonBasis(const juce::String& text);
void tip(juce::Component& c, const juce::String& name, const juce::String& tooltip);

class FlowContent : public juce::Component, private juce::TextEditor::Listener {
public:
    FlowContent();
    ~FlowContent() override;
    struct Item {
        juce::Component* c;
        int basis;
        int height;
        bool full;
        std::function<int(int)> heightFor;
    };
    void addItem(juce::Component& c, int basis, int height, bool full = false);
    void addNote(NoteLabel& n);          // full-width wrapped text
    int layout(int width);               // sets child bounds, returns content height
    void setTheme(const Theme& t);
    const Theme& theme() const noexcept { return theme_; }
    void paint(juce::Graphics& g) override;
    const std::vector<Item>& items() const noexcept { return items_; }

private:
    void syncTextEditors();
    void textEditorTextChanged(juce::TextEditor&) override;
    Theme theme_ = darkTheme();
    juce::LookAndFeel_V4 laf_;
    std::vector<Item> items_;
    std::vector<juce::Component::SafePointer<juce::TextEditor>> textEditors_;
};

const Theme& themeOf(const juce::Component& c);

// Base of every panel: theme, scrolling viewport and flow content.
class PanelBase : public Panel {
public:
    explicit PanelBase(EditorBridge& b);
    ~PanelBase() override;
    void settingsChanged(const UiSettings& s) override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    FlowContent& content() noexcept { return content_; }
    juce::Viewport& viewport() noexcept { return viewport_; }
    void relayout();
    EditorBridge& bridge;

protected:
    virtual void themeChanged() {}
    NoteLabel* heading(const juce::String& text);                    // section heading (full row)
    NoteLabel* note(const juce::String& text, Tone tone = Tone::Dim);  // wrapped text (full row)
    FlowContent content_;
    juce::Viewport viewport_;
    std::vector<std::unique_ptr<NoteLabel>> notes_;
    bool dark_ = true;
    float scale_ = 1.0f;
};

// Plain buttons with tooltip/title.
std::unique_ptr<juce::TextButton> makeButton(const juce::String& text, const juce::String& tooltip);

// Panel factories (one per panel .cpp).
std::unique_ptr<Panel> makePresetsPanel(EditorBridge&);
std::unique_ptr<Panel> makeABPanel(EditorBridge&);
std::unique_ptr<Panel> makeHistoryPanel(EditorBridge&);
std::unique_ptr<Panel> makeLifecyclePanel(EditorBridge&);
std::unique_ptr<Panel> makeFaultsPanel(EditorBridge&);
std::unique_ptr<Panel> makeBlueprintPanel(EditorBridge&);
std::unique_ptr<Panel> makeMusicalPanel(EditorBridge&);
std::unique_ptr<Panel> makeCapturePanel(EditorBridge&);
std::unique_ptr<Panel> makeNetworkPanel(EditorBridge&);
std::unique_ptr<Panel> makeCommunityPanel(EditorBridge&);
std::unique_ptr<Panel> makeQualityPanel(EditorBridge&);

}  // namespace cd::plugin::ui::pc
