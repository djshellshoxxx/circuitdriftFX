#include "ui/panels/PanelCommon.h"
#include "ui/workbench/GestureMath.h"
#include <algorithm>
#include <cmath>

namespace cd::plugin::ui::pc {

namespace {
juce::Font makeFont(float h, bool bold = false) {
    return juce::Font(juce::FontOptions(h, bold ? juce::Font::bold : juce::Font::plain));
}
juce::Colour toneColour(const Theme& t, Tone tone) {
    switch (tone) {
        case Tone::Normal: return t.text;
        case Tone::Dim: return t.textDim;
        case Tone::Warn: return t.warning;
        case Tone::Danger: return t.danger;
        case Tone::Ok: return t.ok;
        case Tone::Accent: return t.accent;
    }
    return t.text;
}
}  // namespace

const Theme& themeOf(const juce::Component& c) {
    if (auto* f = c.findParentComponentOfClass<FlowContent>()) return f->theme();
    return darkTheme();
}

void applyTheme(juce::LookAndFeel_V4& l, const Theme& t) {
    using namespace juce;
    l.setColour(ResizableWindow::backgroundColourId, t.panel);
    l.setColour(Label::textColourId, t.text);
    l.setColour(Label::textWhenEditingColourId, t.text);
    l.setColour(Label::backgroundColourId, Colours::transparentBlack);
    l.setColour(Label::outlineColourId, Colours::transparentBlack);
    l.setColour(TextButton::buttonColourId, t.panelRaised);
    l.setColour(TextButton::buttonOnColourId, t.accent);
    l.setColour(TextButton::textColourOffId, t.text);
    l.setColour(TextButton::textColourOnId, t.textInverse);
    l.setColour(ToggleButton::textColourId, t.text);
    l.setColour(ToggleButton::tickColourId, t.accent);
    l.setColour(ToggleButton::tickDisabledColourId, t.textDim);
    l.setColour(Slider::backgroundColourId, t.inset);
    l.setColour(Slider::trackColourId, t.accent);
    l.setColour(Slider::thumbColourId, t.text);
    l.setColour(Slider::textBoxTextColourId, t.text);
    l.setColour(Slider::textBoxBackgroundColourId, t.inset);
    l.setColour(Slider::textBoxOutlineColourId, t.border);
    l.setColour(Slider::textBoxHighlightColourId, t.accentDim);
    l.setColour(ComboBox::backgroundColourId, t.inset);
    l.setColour(ComboBox::textColourId, t.text);
    l.setColour(ComboBox::outlineColourId, t.border);
    l.setColour(ComboBox::arrowColourId, t.text);
    l.setColour(ComboBox::focusedOutlineColourId, t.focus);
    l.setColour(PopupMenu::backgroundColourId, t.panelRaised);
    l.setColour(PopupMenu::textColourId, t.text);
    l.setColour(PopupMenu::highlightedBackgroundColourId, t.accent);
    l.setColour(PopupMenu::highlightedTextColourId, t.textInverse);
    l.setColour(TextEditor::backgroundColourId, t.inset);
    l.setColour(TextEditor::textColourId, t.text);
    l.setColour(TextEditor::outlineColourId, t.border);
    l.setColour(TextEditor::focusedOutlineColourId, t.focus);
    l.setColour(TextEditor::highlightColourId, t.accentDim);
    l.setColour(TextEditor::highlightedTextColourId, t.text);
    l.setColour(CaretComponent::caretColourId, t.accent);
    l.setColour(ListBox::backgroundColourId, t.inset);
    l.setColour(ListBox::outlineColourId, t.border);
    l.setColour(ListBox::textColourId, t.text);
    l.setColour(ScrollBar::thumbColourId, t.border);
    l.setColour(ScrollBar::backgroundColourId, t.inset);
    l.setColour(TooltipWindow::backgroundColourId, t.panelRaised);
    l.setColour(TooltipWindow::textColourId, t.text);
    l.setColour(TooltipWindow::outlineColourId, t.border);
    l.setColour(AlertWindow::backgroundColourId, t.panelRaised);
    l.setColour(AlertWindow::textColourId, t.text);
}

int textWidth(const juce::String& text, float height, bool bold) {
    juce::GlyphArrangement ga;
    ga.addLineOfText(makeFont(height, bold), text, 0.0f, 0.0f);
    return static_cast<int>(std::ceil(ga.getBoundingBox(0, -1, false).getWidth()));
}
int buttonBasis(const juce::String& text) { return textWidth(text, 15.0f) + 28; }

void tip(juce::Component& c, const juce::String& name, const juce::String& tooltip) {
    c.setTitle(name);
    c.setDescription(tooltip);
    if (auto* t = dynamic_cast<juce::SettableTooltipClient*>(&c)) t->setTooltip(tooltip);
}

std::unique_ptr<juce::TextButton> makeButton(const juce::String& text, const juce::String& tooltip) {
    auto b = std::make_unique<juce::TextButton>(text);
    tip(*b, text, tooltip);
    return b;
}

// ---------------------------------------------------------------------------------- NoteLabel
bool NoteLabel::set(const juce::String& text, Tone tone) {
    if (text == text_ && tone == tone_) return false;
    const bool resize = text != text_;
    text_ = text;
    tone_ = tone;
    setVisible(text_.isNotEmpty());
    repaint();
    return resize;
}

juce::AttributedString NoteLabel::build(int) const {
    const Theme& t = themeOf(*this);
    juce::AttributedString a;
    a.setJustification(juce::Justification::topLeft);
    a.setWordWrap(juce::AttributedString::byWord);
    a.append(text_, makeFont(size_ > 0 ? size_ : t.textSmall + 1.0f, bold_), toneColour(t, tone_));
    return a;
}

int NoteLabel::heightFor(int width) const {
    if (text_.isEmpty()) return 0;
    juce::TextLayout l;
    l.createLayout(build(width), static_cast<float>(std::max(20, width)));
    return static_cast<int>(std::ceil(l.getHeight())) + 4;
}

void NoteLabel::paint(juce::Graphics& g) {
    if (text_.isEmpty()) return;
    juce::TextLayout l;
    l.createLayout(build(getWidth()), static_cast<float>(getWidth()));
    l.draw(g, getLocalBounds().toFloat().withTrimmedTop(1.0f));
}

// ------------------------------------------------------------------------------- ParamBinding
void ParamBinding::bind(const juce::String& paramId, std::function<void(float)> cb) {
    if (paramId == id_ && param_ != nullptr) return;
    unbind();
    param_ = bridge_.parameters().getParameter(paramId);
    if (param_ == nullptr) return;
    id_ = paramId;
    attach_ = std::make_unique<juce::ParameterAttachment>(*param_, std::move(cb));
    attach_->sendInitialUpdate();
}
void ParamBinding::setComplete(float v) {
    if (!attach_ || !std::isfinite(v)) return;
    if (inPart_) { setPart(v); return; }
    bridge_.beginGesture(label_);
    attach_->setValueAsCompleteGesture(v);
    bridge_.endGesture();
}
void ParamBinding::beginPart() {
    if (!attach_ || inPart_) return;
    inPart_ = true;
    bridge_.beginGesture(label_);
    attach_->beginGesture();
}
void ParamBinding::setPart(float v) {
    if (attach_ && inPart_ && std::isfinite(v)) attach_->setValueAsPartOfGesture(v);
}
void ParamBinding::endPart() {
    if (!attach_ || !inPart_) return;
    attach_->endGesture();
    bridge_.endGesture();
    inPart_ = false;
}
float ParamBinding::value() const { return param_ ? param_->convertFrom0to1(param_->getValue()) : 0.0f; }

// -------------------------------------------------------------------------------- ParamSlider
ParamSlider::ParamSlider(EditorBridge& b, const juce::String& name, const juce::String& tooltip)
    : binding(b, "Edit " + name) {
    label.setText(name, juce::dontSendNotification);
    label.setFont(makeFont(12.5f));
    label.setMinimumHorizontalScale(1.0f);
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 84, 22);
    slider.setRange(0.0, 1.0, 0.0);
    slider.setWantsKeyboardFocus(true);
    slider.setScrollWheelEnabled(true);
    addAndMakeVisible(slider);
    tip(slider, name, tooltip);
    slider.onDragStart = [this] { inPart_ = true; binding.beginPart(); };
    slider.onValueChange = [this] {
        if (updating_ || !binding.bound()) return;
        const float v = binding.parameter()->convertFrom0to1(static_cast<float>(slider.getValue()));
        if (inPart_) binding.setPart(v); else binding.setComplete(v);
    };
    slider.onDragEnd = [this] { binding.endPart(); inPart_ = false; };
    slider.textFromValueFunction = [this](double v) {
        if (!binding.bound()) return juce::String("n/a");
        const double d = binding.parameter()->convertFrom0to1(static_cast<float>(v));
        if (fmt_) return fmt_(d);
        const auto& rg = binding.parameter()->getNormalisableRange();
        const double span = std::abs(static_cast<double>(rg.end - rg.start));
        auto s = juce::String(d, span < 10.0 ? 2 : span < 1000.0 ? 1 : 0);
        auto unit = binding.parameter()->getLabel();
        if (unit == "normalized" || unit == "index") unit = {};
        return unit.isNotEmpty() && !s.endsWithIgnoreCase(unit) ? s + " " + unit : s;
    };
    slider.valueFromTextFunction = [this](const juce::String& t) {
        const double current = slider.getValue();
        if (!binding.bound() || t.trim() == slider.getTextFromValue(current).trim()) return current;
        const auto& range = binding.parameter()->getNormalisableRange();
        const auto value = parse_ ? parse_(t.toStdString())
                                  : gesture::parseBoundedQuantity(t.toStdString(), binding.parameter()->getLabel().toStdString(),
                                                                  range.start, range.end);
        if (!value || !std::isfinite(*value) || *value < range.start || *value > range.end) return current;
        return static_cast<double>(binding.parameter()->convertTo0to1(static_cast<float>(*value)));
    };
}

ParamSlider::~ParamSlider() {
    // JUCE can end an internal drag while destroying the slider. Its callbacks
    // must not refer to the binding/formatters that are destroyed before it.
    slider.onDragStart = {};
    slider.onDragEnd = {};
    slider.onValueChange = {};
    slider.textFromValueFunction = {};
    slider.valueFromTextFunction = {};
    binding.unbind();
}

void ParamSlider::setDisplay(std::function<juce::String(double)> f, PlainParser parser) {
    fmt_ = std::move(f);
    parse_ = std::move(parser);
    slider.updateText();
}

void ParamSlider::bind(const juce::String& id) {
    binding.bind(id, [this](float v) {
        if (!binding.bound()) return;
        updating_ = true;
        slider.setValue(binding.parameter()->convertTo0to1(v), juce::dontSendNotification);
        slider.updateText();
        updating_ = false;
    });
    slider.setEnabled(binding.bound());
    if (!binding.bound()) slider.setValue(0.0, juce::dontSendNotification);
    slider.updateText();
}

void ParamSlider::resized() {
    auto r = getLocalBounds();
    label.setBounds(r.removeFromTop(18));
    slider.setBounds(r.removeFromTop(26));
}

// -------------------------------------------------------------------------------- ParamChoice
ParamChoice::ParamChoice(EditorBridge& b, const juce::String& name, const juce::String& tooltip)
    : binding(b, "Set " + name) {
    label.setText(name, juce::dontSendNotification);
    label.setFont(makeFont(12.5f));
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
    addAndMakeVisible(box);
    tip(box, name, tooltip);
    box.onChange = [this] {
        if (updating_ || !binding.bound() || box.getSelectedId() < 1) return;
        binding.setComplete(static_cast<float>(box.getSelectedId() - 1));
    };
}

void ParamChoice::bind(const juce::String& id, const juce::StringArray& labels) {
    binding.unbind();
    box.clear(juce::dontSendNotification);
    auto* ch = dynamic_cast<juce::AudioParameterChoice*>(binding.lookup(id));
    if (ch != nullptr) {
        for (int i = 0; i < ch->choices.size(); ++i) box.addItem(i < labels.size() ? labels[i] : ch->choices[i], i + 1);
    }
    binding.bind(id, [this](float v) {
        updating_ = true;
        box.setSelectedId(static_cast<int>(std::lround(v)) + 1, juce::dontSendNotification);
        updating_ = false;
    });
    box.setEnabled(binding.bound());
}

void ParamChoice::resized() {
    auto r = getLocalBounds();
    label.setBounds(r.removeFromTop(18));
    box.setBounds(r.removeFromTop(28));
}

// -------------------------------------------------------------------------------- ParamToggle
ParamToggle::ParamToggle(EditorBridge& b, const juce::String& text, const juce::String& tooltip)
    : juce::ToggleButton(text), binding(b, "Toggle " + text) {
    tip(*this, text, tooltip);
    setWantsKeyboardFocus(true);
    onClick = [this] {
        if (updating_ || !binding.bound()) return;
        binding.setComplete(getToggleState() ? 1.0f : 0.0f);
        if (onUserChange) onUserChange(getToggleState());
    };
}
void ParamToggle::bind(const juce::String& id) {
    binding.bind(id, [this](float v) {
        updating_ = true;
        setToggleState(v >= 0.5f, juce::dontSendNotification);
        updating_ = false;
    });
    setEnabled(binding.bound());
}

// ---------------------------------------------------------------------------------- FlowContent
FlowContent::FlowContent() {
    applyTheme(laf_, theme_);
    setLookAndFeel(&laf_);
    setOpaque(true);
}
void FlowContent::setTheme(const Theme& t) {
    theme_ = t;
    applyTheme(laf_, t);
    sendLookAndFeelChange();
    repaint();
}
void FlowContent::paint(juce::Graphics& g) { g.fillAll(theme_.panel); }

void FlowContent::addItem(juce::Component& c, int basis, int height, bool full) {
    addAndMakeVisible(c);
    items_.push_back({&c, basis, height, full, {}});
}
void FlowContent::addNote(NoteLabel& n) {
    addChildComponent(n);
    NoteLabel* p = &n;
    items_.push_back({&n, 200, 0, true, [p](int w) { return p->heightFor(w); }});
}

int FlowContent::layout(int width) {
    const int g = theme_.gap, pad = theme_.padding;
    const int availW = std::max(120, width - 2 * pad);
    int y = pad;
    bool any = false;
    std::size_t i = 0;
    const auto n = items_.size();
    while (i < n) {
        if (!items_[i].c->isVisible()) { ++i; continue; }
        std::vector<std::size_t> line;
        int sum = 0;
        for (; i < n; ++i) {
            const auto& it = items_[i];
            if (!it.c->isVisible()) continue;
            const int b = std::min(it.basis, availW);
            if (it.full) {
                if (!line.empty()) break;
                line.push_back(i);
                ++i;
                break;
            }
            const int need = sum + (line.empty() ? 0 : g) + b;
            if (!line.empty() && need > availW) break;
            sum = need;
            line.push_back(i);
        }
        int basisSum = 0;
        for (auto k : line) basisSum += items_[k].full ? availW : std::min(items_[k].basis, availW);
        const int extra = std::max(0, availW - (sum > 0 ? sum : basisSum));
        int x = pad, lineH = 0, used = 0;
        for (std::size_t j = 0; j < line.size(); ++j) {
            const auto& it = items_[line[j]];
            int w = it.full ? availW : std::min(it.basis, availW);
            if (!it.full && basisSum > 0) w += (j + 1 == line.size()) ? (availW - x + pad - w) : extra * w / basisSum;
            w = std::max(1, std::min(w, availW));
            const int h = it.heightFor ? it.heightFor(w) : it.height;
            it.c->setBounds(x, y, w, h);
            x += w + g;
            used += w;
            lineH = std::max(lineH, h);
        }
        (void) used;
        y += lineH + g;
        any = true;
    }
    return any ? y - g + pad : 2 * pad;
}

// ------------------------------------------------------------------------------------ PanelBase
PanelBase::PanelBase(EditorBridge& b) : bridge(b) {
    dark_ = b.uiSettings().darkTheme;
    scale_ = b.uiSettings().scale;
    content_.setTheme(themeFor(dark_));
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(10);
    viewport_.setWantsKeyboardFocus(false);
    setOpaque(true);
}
PanelBase::~PanelBase() { viewport_.setViewedComponent(nullptr, false); }

void PanelBase::settingsChanged(const UiSettings& s) {
    dark_ = s.darkTheme;
    scale_ = s.scale;
    content_.setTheme(themeFor(dark_));
    themeChanged();
    repaint();
    relayout();
}
void PanelBase::paint(juce::Graphics& g) { g.fillAll(content_.theme().panel); }

void PanelBase::relayout() {
    const int h = getHeight();
    if (getWidth() <= 0) return;
    int w = getWidth();
    int ch = content_.layout(w);
    if (ch > h) {
        w = getWidth() - viewport_.getScrollBarThickness();
        ch = content_.layout(w);
    }
    content_.setSize(w, std::max(ch, 1));
    content_.layout(w);
}
void PanelBase::resized() {
    viewport_.setBounds(getLocalBounds());
    relayout();
}

NoteLabel* PanelBase::heading(const juce::String& text) {
    notes_.push_back(std::make_unique<NoteLabel>(Tone::Accent, content_.theme().textBody, true));
    auto* n = notes_.back().get();
    n->set(text, Tone::Accent);
    content_.addNote(*n);
    return n;
}
NoteLabel* PanelBase::note(const juce::String& text, Tone tone) {
    notes_.push_back(std::make_unique<NoteLabel>(tone));
    auto* n = notes_.back().get();
    n->set(text, tone);
    content_.addNote(*n);
    return n;
}

}  // namespace cd::plugin::ui::pc
