#include "ui/shell/EditorCore.h"

#include <algorithm>
#include <array>
#include <functional>
#include "Edition.h"
#include "ui/shell/ShellWidgets.h"
#include "ui/workbench/WorkbenchView.h"

namespace cd::plugin::ui::shell {

namespace {

constexpr int kHeaderH = 48, kStatusH = 24, kMargin = 10;
constexpr int kPanelCount = static_cast<int>(PanelId::Count);

bool sameSettings(const UiSettings& a, const UiSettings& b) noexcept {
    return a.scale == b.scale && a.darkTheme == b.darkTheme && a.view == b.view &&
           a.educational == b.educational && a.reduceMotion == b.reduceMotion &&
           a.lowFrameRate == b.lowFrameRate && a.overlay == b.overlay &&
           a.xray == b.xray && a.pcbView == b.pcbView &&
           a.lastWidth == b.lastWidth && a.lastHeight == b.lastHeight;
}

// ---- Quick scenes (UX-02 table). Versioned creative factory scenes, 100 % wet, Auto-Gain on. -------
struct SceneDef { const char* name; double age, sag, drift, warmth; bool noise; const char* note; };
constexpr SceneDef kScenes[] = {
    {"Clean Console", 0.0, 0.0, 0.0, 0.0, false, "Calibrated clean base: all four macros at zero, analog noise off."},
    {"Warm Tube Preamp", 0.15, 0.2, 0.1, 0.55, false, "Moderate tube drive with a little age, sag and drift."},
    {"Pumping Bus Glue", 0.1, 0.8, 0.1, 0.35, false, "Sag-driven coloration: the supply breathes with the music."},
    {"Lo-Fi Attic Gear", 0.85, 0.65, 0.6, 0.45, true, "Intentionally degraded: heavy age, sag and drift with analog noise on."},
};

// ---- Look and feel (popup menus, text entry) --------------------------------------------------------
class ShellLookAndFeel : public juce::LookAndFeel_V4 {
public:
    void apply(const Theme& t) {
        using PM = juce::PopupMenu;
        setColour(PM::backgroundColourId, t.panelRaised);
        setColour(PM::textColourId, t.text);
        setColour(PM::headerTextColourId, t.textDim);
        setColour(PM::highlightedBackgroundColourId, t.accent);
        setColour(PM::highlightedTextColourId, t.textInverse);
        setColour(juce::TextEditor::backgroundColourId, t.inset);
        setColour(juce::TextEditor::textColourId, t.text);
        setColour(juce::TextEditor::outlineColourId, t.border);
        setColour(juce::TextEditor::focusedOutlineColourId, t.focus);
        setColour(juce::ResizableWindow::backgroundColourId, t.background);
    }
    juce::Font getPopupMenuFont() override { return uiFont(13.0f); }
};

// ---- Root: brushed metal background, shortcut keys -------------------------------------------------
class ShellRoot : public juce::Component {
public:
    explicit ShellRoot(ShellContext& c) : ctx(c) {
        setFocusContainerType(FocusContainerType::keyboardFocusContainer);
        setTitle("CircuitDrift editor");
    }
    void rebuildBackground(float pixelScale) {
        const auto& t = ctx.theme();
        const float k = std::clamp(pixelScale, 1.0f, 2.0f);
        bg = makeBrushedBackground(juce::roundToInt(getWidth() * k), juce::roundToInt(getHeight() * k), t);
        bgDark = t.dark;
    }
    void paint(juce::Graphics& g) override {
        if (bg.isValid() && bgDark == ctx.theme().dark)
            g.drawImage(bg, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
        else
            g.fillAll(ctx.theme().background);
    }
    bool keyPressed(const juce::KeyPress& k) override { return onKey ? onKey(k) : false; }
    ShellContext& ctx;
    juce::Image bg;
    bool bgDark = true;
    std::function<bool(const juce::KeyPress&)> onKey;
};

// ---- Status bar (quality badge + status badges) ---------------------------------------------------
class StatusBar : public juce::Component {
public:
    enum class Kind { Info, Warn, Danger };
    struct Badge { juce::String text; Kind kind; };
    explicit StatusBar(ShellContext& c) : ctx(c) { setTitle("Status"); setTip(*this, "Status", "Processing quality, latency and status flags."); }
    void set(const juce::String& quality, std::vector<Badge> b) {
        if (quality == qualityText && sameBadges(b)) return;
        qualityText = quality;
        badges = std::move(b);
        juce::String d = quality;
        for (auto& x : badges) d << ", " << x.text;
        setDescription(d);
        repaint();
    }
    bool sameBadges(const std::vector<Badge>& b) const {
        if (b.size() != badges.size()) return false;
        for (size_t i = 0; i < b.size(); ++i)
            if (b[i].text != badges[i].text || b[i].kind != badges[i].kind) return false;
        return true;
    }
    void paint(juce::Graphics& g) override {
        const auto& t = ctx.theme();
        auto r = getLocalBounds().toFloat().reduced(static_cast<float>(kMargin), 2.0f);
        g.setColour(t.inset);
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(t.border.withAlpha(0.5f));
        g.drawRoundedRectangle(r, 4.0f, 1.0f);
        auto in = r.reduced(8.0f, 0.0f);
        const auto qf = monoFont(11.5f, true);
        const float qw = std::min(in.getWidth() * 0.70f, textWidth(qf, qualityText) + 26.0f);
        qualityRect = in.removeFromLeft(qw).toNearestInt();
        // Quality indicator: square LED + text (clickable: opens the Quality panel).
        g.setColour(t.accent);
        g.fillRect(juce::Rectangle<float>(static_cast<float>(qualityRect.getX()), r.getCentreY() - 3.0f, 6.0f, 6.0f));
        g.setFont(qf);
        g.setColour(t.text);
        g.drawText(qualityText, qualityRect.withTrimmedLeft(12).toFloat(), juce::Justification::centredLeft, true);

        // Badges, right aligned; those that do not fit collapse into "+N".
        float x = in.getRight();
        int hidden = 0;
        const auto bf = uiFont(10.5f, true).withExtraKerningFactor(0.04f);
        for (int i = static_cast<int>(badges.size()) - 1; i >= 0; --i) {
            const auto& b = badges[static_cast<size_t>(i)];
            const float w = textWidth(bf, b.text.toUpperCase()) + 26.0f;
            if (x - w < in.getX() + 8.0f) { ++hidden; continue; }
            juce::Rectangle<float> br(x - w, r.getY() + 3.0f, w, r.getHeight() - 6.0f);
            const auto col = b.kind == Kind::Danger ? t.danger : b.kind == Kind::Warn ? t.warning : t.textDim;
            g.setColour(col);
            g.drawRoundedRectangle(br, 3.0f, 1.2f);
            // Shape cue (triangle / square / circle) so state never relies on colour.
            juce::Path s;
            const auto cp = juce::Point<float>(br.getX() + 9.0f, br.getCentreY());
            if (b.kind == Kind::Danger) s.addRectangle(cp.x - 3.0f, cp.y - 3.0f, 6.0f, 6.0f);
            else if (b.kind == Kind::Warn) s.addTriangle(cp.x - 4.0f, cp.y + 3.5f, cp.x + 4.0f, cp.y + 3.5f, cp.x, cp.y - 4.0f);
            else s.addEllipse(cp.x - 3.0f, cp.y - 3.0f, 6.0f, 6.0f);
            g.fillPath(s);
            g.setFont(bf);
            g.drawText(b.text.toUpperCase(), br.withTrimmedLeft(17.0f).withTrimmedRight(4.0f), juce::Justification::centredLeft, false);
            x -= w + 6.0f;
        }
        if (hidden > 0) {
            g.setFont(monoFont(11.0f, true));
            g.setColour(t.warning);
            g.drawText("+" + juce::String(hidden), juce::Rectangle<float>(x - 30.0f, r.getY(), 30.0f, r.getHeight()), juce::Justification::centredRight, false);
        }
    }
    void mouseUp(const juce::MouseEvent& e) override {
        if (qualityRect.contains(e.getPosition()) && onQualityClick) onQualityClick();
    }
    ShellContext& ctx;
    juce::String qualityText;
    std::vector<Badge> badges;
    juce::Rectangle<int> qualityRect;
    std::function<void()> onQualityClick;
};

// ---- Header ------------------------------------------------------------------------------------------
class HeaderBar : public juce::Component {
public:
    HeaderBar(ShellContext& c, bool liteEdition)
        : ctx(c), lite(liteEdition),
          presetChip(c, "Preset"), aChip(c, "A"), bChip(c, "B"), copyChip(c, "", Glyph::Copy), swapChip(c, "", Glyph::Swap),
          undoChip(c, "Undo", Glyph::Undo), redoChip(c, "Redo", Glyph::Redo), easyChip(c, "Easy"), workbenchChip(c, "Workbench"),
          panelsChip(c, "", Glyph::Panels), gearChip(c, "", Glyph::Gear) {
        setTitle("Header");
        setFocusContainerType(FocusContainerType::none);
        for (ChipButton* b : chips()) addAndMakeVisible(*b);
        presetChip.setTitle("Preset browser");
        setTip(presetChip, "Preset", "Current preset. Opens the preset browser. 'Modified' appears when settings differ from the loaded preset.");
        setTip(aChip, "Edit slot A", "Detailed controls edit circuit A. Both circuits always run; the A/B crossfade blends them.");
        setTip(bChip, "Edit slot B", "Detailed controls edit circuit B.");
        copyChip.setTitle("Copy this slot to the other slot");
        setTip(copyChip, "Copy slot", "Copies the slot being edited onto the other slot. One undo step.");
        swapChip.setTitle("Swap slots A and B");
        setTip(swapChip, "Swap A and B", "Exchanges the contents of slots A and B. One undo step.");
        setTip(easyChip, "Easy view", "Tone State Sphere, four macros and quick scenes. Same sound state as Workbench.");
        setTip(workbenchChip, "Workbench view", "Component-level inspector, plots and lifecycle tools. Same sound state as Easy view.");
        panelsChip.setTitle("Panels drawer");
        setTip(panelsChip, "Panels", "Presets, A/B, history, lifecycle, faults, blueprint, musical, capture, network, community and quality panels.");
        gearChip.setTitle("Settings");
        setTip(gearChip, "Settings", "Theme, scale 75 to 200 percent, educational tooltips, reduce motion and frame rate. Saved with the session, never automated.");
        setTip(undoChip, "Undo", "Undo the last change.");
        setTip(redoChip, "Redo", "Redo the last undone change.");
        if (lite) {
            for (ChipButton* b : {&presetChip, &aChip, &bChip, &copyChip, &swapChip, &undoChip, &redoChip, &easyChip, &workbenchChip, &panelsChip})
                b->setVisible(false);
        }
    }
    std::array<ChipButton*, 11> chips() {
        return {&presetChip, &aChip, &bChip, &copyChip, &swapChip, &undoChip, &redoChip, &easyChip, &workbenchChip, &panelsChip, &gearChip};
    }
    void paint(juce::Graphics& g) override {
        const auto& t = ctx.theme();
        auto r = getLocalBounds().toFloat();
        g.setGradientFill(juce::ColourGradient(t.panelRaised, 0, 0, t.panel, 0, r.getBottom(), false));
        g.fillRect(r);
        g.setColour(t.accent);
        g.fillRect(r.withTop(r.getBottom() - 2.0f));
        g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.5f : 0.15f));
        g.fillRect(r.withTop(r.getBottom() - 3.0f).withHeight(1.0f));
        // Product name, engraved, with the edition underneath.
        auto nr = juce::Rectangle<float>(static_cast<float>(kMargin), 5.0f, 170.0f, 26.0f);
        const auto f = uiFont(19.0f, true).withExtraKerningFactor(0.10f);
        const float w1 = textWidth(f, "CIRCUIT");
        drawEngraved(g, "CIRCUIT", nr, f, t, juce::Justification::centredLeft, t.text);
        drawEngraved(g, "DRIFT", nr.withTrimmedLeft(w1 + 1.0f), f, t, juce::Justification::centredLeft, t.accent);
        drawEngraved(g, lite ? "LITE EDITION" : "ANALOG CIRCUIT DRIFT", juce::Rectangle<float>(nr.getX(), 30.0f, 170.0f, 14.0f),
                     uiFont(9.5f, true).withExtraKerningFactor(0.10f), t, juce::Justification::centredLeft, t.textDim);
        drawScrew(g, {r.getRight() - 6.0f, 6.0f}, 2.6f, t, 0.6f);
    }
    void layout(bool narrow) {
        const int W = getWidth(), y = 10, h = 28, gap = 6;
        int x = W - kMargin;
        auto place = [&](juce::Component& c, int w) { x -= w; c.setBounds(x, y, w, h); x -= gap; };
        place(gearChip, 32);
        if (lite) return;
        place(panelsChip, 32);
        x -= 4;
        place(workbenchChip, workbenchChip.preferredWidth());
        x += gap - 1;  // segmented pair
        place(easyChip, easyChip.preferredWidth());
        x -= 4;
        const int uw = narrow ? 32 : undoChip.preferredWidth();
        place(redoChip, uw);
        place(undoChip, uw);
        x -= 4;
        place(swapChip, 32);
        place(copyChip, 32);
        place(bChip, 30);
        x += gap - 2;
        place(aChip, 30);
        x -= 6;
        const int left = kMargin + 176;
        presetChip.setBounds(left, y, std::max(60, x - left + gap), h);
    }
    ShellContext& ctx;
    bool lite;
    ChipButton presetChip, aChip, bChip, copyChip, swapChip, undoChip, redoChip, easyChip, workbenchChip, panelsChip, gearChip;
};

// ---- Macro row -----------------------------------------------------------------------------------------
class MacroRow : public juce::Component {
public:
    MacroRow(ShellContext& c, EditorBridge& b, bool lite) : ctx(c) {
        setTitle("Macros");
        setFocusContainerType(FocusContainerType::none);
        auto add = [&](const char* id, const char* label) {
            auto k = std::make_unique<Knob>(c, b, id, label, Fmt::Percent);
            if (!k->isAttached()) return;
            addAndMakeVisible(*k);
            knobs.push_back(std::move(k));
        };
        if (lite) {
            add("lite.sag", "Sag");
            add("lite.thermalAge", "Thermal Age");
        } else {
            add("macro.age", "Age");
            add("macro.sag", "Sag");
            add("macro.drift", "Drift");
            add("macro.warmth", "Warmth");
        }
    }
    void paint(juce::Graphics& g) override {
        const auto& t = ctx.theme();
        auto r = getLocalBounds().toFloat();
        drawPlate(g, r, t, true, t.corner);
        drawScrew(g, {r.getX() + 8.0f, r.getY() + 8.0f}, 2.8f, t, 0.3f);
        drawScrew(g, {r.getRight() - 8.0f, r.getY() + 8.0f}, 2.8f, t, 1.1f);
        drawScrew(g, {r.getX() + 8.0f, r.getBottom() - 8.0f}, 2.8f, t, 2.0f);
        drawScrew(g, {r.getRight() - 8.0f, r.getBottom() - 8.0f}, 2.8f, t, 0.8f);
    }
    void resized() override {
        if (knobs.empty()) return;
        auto r = getLocalBounds().reduced(22, 6);
        const int n = static_cast<int>(knobs.size());
        const int cell = r.getWidth() / n;
        const bool compact = getHeight() < 96;
        for (int i = 0; i < n; ++i) {
            auto c = r.removeFromLeft(cell);
            if (compact) {
                knobs[static_cast<size_t>(i)]->setBounds(c.withSizeKeepingCentre(std::min(c.getWidth() - 8, 150), std::min(c.getHeight(), 56)));
            } else {
                const int w = std::min(c.getWidth() - 10, 150);
                knobs[static_cast<size_t>(i)]->setBounds(c.withSizeKeepingCentre(w, c.getHeight()));
            }
        }
    }
    ShellContext& ctx;
    std::vector<std::unique_ptr<Knob>> knobs;
};

// ---- Utilities strip -------------------------------------------------------------------------------------
class UtilityStrip : public juce::Component {
public:
    UtilityStrip(ShellContext& c, EditorBridge& b, bool liteEdition) : ctx(c), lite(liteEdition) {
        setTitle("Utilities");
        setFocusContainerType(FocusContainerType::none);
        auto addKnob = [&](const char* id, const char* label, Fmt f) {
            auto k = std::make_unique<Knob>(c, b, id, label, f);
            if (!k->isAttached()) return;
            addAndMakeVisible(*k);
            knobs.push_back(std::move(k));
        };
        auto addSwitch = [&](const char* id, const char* label) {
            auto s = std::make_unique<Switch>(c, b, id, label);
            if (!s->isAttached()) return;
            addAndMakeVisible(*s);
            switches.push_back(std::move(s));
        };
        if (!lite) {
            addKnob("input.gain", "Input", Fmt::Db);
            addKnob("mix", "Mix", Fmt::Percent);
            addKnob("output.gain", "Output", Fmt::Db);
            addKnob("hpf.frequency", "HPF", Fmt::Hz);
            addSwitch("gainMatch.enabled", "Auto-Gain");
            addSwitch("hpf.enabled", "HPF");
            addSwitch("output.softClip", "Soft Clip");
            addSwitch("analogNoise.enabled", "Noise");
            // The full edition has no wrapper bypass parameter: the host owns bypass. The indicator
            // stays reachable (focusable, with help) and shows the live state.
            bypass = std::make_unique<Switch>(c, b, juce::String(), "Bypass");
            bypass->setIndicator(false, "ACTIVE", "OFF");
            bypass->setTitle("Bypass (host)");
            setTip(*bypass, "Bypass", "Shows whether the host has bypassed the plugin. Use the host's own bypass control to change it.");
            addAndMakeVisible(*bypass);
        } else {
            bypass = std::make_unique<Switch>(c, b, kBypassParamId, "Bypass");
            if (!bypass->isAttached()) bypass->setIndicator(false, "ACTIVE", "OFF");
            addAndMakeVisible(*bypass);
        }
    }
    void paint(juce::Graphics& g) override {
        const auto& t = ctx.theme();
        auto r = getLocalBounds().toFloat().reduced(static_cast<float>(kMargin), 4.0f);
        drawPlate(g, r, t, false, t.corner);
        drawScrew(g, {r.getX() + 8.0f, r.getY() + 8.0f}, 2.6f, t, 0.9f);
        drawScrew(g, {r.getRight() - 8.0f, r.getY() + 8.0f}, 2.6f, t, 0.1f);
        drawScrew(g, {r.getX() + 8.0f, r.getBottom() - 8.0f}, 2.6f, t, 1.7f);
        drawScrew(g, {r.getRight() - 8.0f, r.getBottom() - 8.0f}, 2.6f, t, 2.4f);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(kMargin + 18, 8);
        if (lite) {
            bypass->setBounds(r.removeFromLeft(130).withSizeKeepingCentre(130, 30));
            return;
        }
        const int gridW = std::clamp(static_cast<int>(r.getWidth() * 0.5), 330, 520);
        auto grid = r.removeFromRight(gridW);
        r.removeFromRight(12);
        const int n = static_cast<int>(knobs.size());
        const int cell = n > 0 ? r.getWidth() / n : 0;
        for (auto& k : knobs) k->setBounds(r.removeFromLeft(cell).withSizeKeepingCentre(std::min(cell - 4, 96), r.getHeight()));
        // 3 columns x 2 rows of pills.
        const int cols = 3, gap = 6;
        const int pw = (grid.getWidth() - gap * (cols - 1)) / cols;
        const int ph = (grid.getHeight() - gap) / 2;
        std::vector<juce::Component*> pills;
        for (auto& s : switches) pills.push_back(s.get());
        pills.push_back(bypass.get());
        for (size_t i = 0; i < pills.size(); ++i) {
            const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
            pills[i]->setBounds(grid.getX() + col * (pw + gap), grid.getY() + row * (ph + gap), pw, ph);
        }
    }
    ShellContext& ctx;
    bool lite;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Switch>> switches;
    std::unique_ptr<Switch> bypass;
};

// ---- Easy view: sphere + quick scenes --------------------------------------------------------------------
class EasyView : public juce::Component {
public:
    EasyView(ShellContext& c, bool lite) : ctx(c), sphere(c) {
        setTitle("Easy view");
        setFocusContainerType(FocusContainerType::none);
        addAndMakeVisible(sphere);
        if (lite) return;
        for (int i = 0; i < static_cast<int>(std::size(kScenes)); ++i) {
            auto chip = std::make_unique<ChipButton>(c, kScenes[i].name);
            setTip(*chip, kScenes[i].name, kScenes[i].note);
            chip->setTitle(juce::String("Scene ") + kScenes[i].name);
            addAndMakeVisible(*chip);
            scenes.push_back(std::move(chip));
        }
    }
    void paint(juce::Graphics& g) override {
        const auto& t = ctx.theme();
        auto r = getLocalBounds().toFloat();
        drawPlate(g, r, t, false, t.corner);
        if (!scenes.empty()) {
            auto row = r.reduced(14.0f, 0.0f).removeFromBottom(static_cast<float>(kSceneRow) + 8.0f).withTrimmedBottom(8.0f);
            drawEngraved(g, "QUICK SCENES", row.removeFromLeft(96.0f), uiFont(10.0f, true).withExtraKerningFactor(0.08f), t,
                         juce::Justification::centredLeft, t.textDim);
        }
        drawScrew(g, {r.getX() + 8.0f, r.getY() + 8.0f}, 2.8f, t, 0.5f);
        drawScrew(g, {r.getRight() - 8.0f, r.getY() + 8.0f}, 2.8f, t, 1.4f);
        drawEngraved(g, "TONE STATE", juce::Rectangle<float>(r.getCentreX() - 60.0f, r.getY() + 4.0f, 120.0f, 14.0f),
                     uiFont(10.0f, true).withExtraKerningFactor(0.12f), t, juce::Justification::centred, t.textDim);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(10, 6);
        r.removeFromTop(14);
        if (!scenes.empty()) {
            auto row = r.removeFromBottom(kSceneRow);
            r.removeFromBottom(6);
            row.removeFromLeft(96);
            const int n = static_cast<int>(scenes.size());
            const int gap = 8;
            const int w = std::min(170, (row.getWidth() - gap * (n - 1)) / n);
            for (auto& s : scenes) { s->setBounds(row.removeFromLeft(w)); row.removeFromLeft(gap); }
        }
        sphere.setBounds(r);
    }
    static constexpr int kSceneRow = 28;
    ShellContext& ctx;
    ToneSphere sphere;
    std::vector<std::unique_ptr<ChipButton>> scenes;
};

// ---- Drawer: tabs + lazily created panels ----------------------------------------------------------------
class Drawer : public juce::Component {
public:
    Drawer(ShellContext& c, EditorBridge& b) : ctx(c), bridge(b), closeChip(c, "", Glyph::Close) {
        setTitle("Panels");
        setFocusContainerType(FocusContainerType::none);
        for (int i = 0; i < kPanelCount; ++i) {
            auto chip = std::make_unique<ChipButton>(c, panelTitle(static_cast<PanelId>(i)));
            chip->setTitle(juce::String(panelTitle(static_cast<PanelId>(i))) + " tab");
            chip->onClick = [this, i] { select(static_cast<PanelId>(i)); };
            addAndMakeVisible(*chip);
            tabs.push_back(std::move(chip));
        }
        closeChip.setTitle("Close panels drawer");
        setTip(closeChip, "Close panels", "Hides the drawer. Escape also closes it when it covers the view.");
        addAndMakeVisible(closeChip);
        select(PanelId::Presets);
    }
    void select(PanelId id) {
        current = id;
        const int idx = static_cast<int>(id);
        if (!panels[static_cast<size_t>(idx)]) {
            panels[static_cast<size_t>(idx)] = createPanel(id, bridge);
            if (auto* p = panels[static_cast<size_t>(idx)].get()) {
                addChildComponent(*p);
                p->settingsChanged(ctx.ui);
            }
        }
        for (int i = 0; i < kPanelCount; ++i) {
            tabs[static_cast<size_t>(i)]->setToggleState(i == idx, juce::dontSendNotification);
            if (panels[static_cast<size_t>(i)]) panels[static_cast<size_t>(i)]->setVisible(i == idx);
        }
        resized();
        repaint();
    }
    Panel* currentPanel() { return panels[static_cast<size_t>(static_cast<int>(current))].get(); }
    void paint(juce::Graphics& g) override {
        const auto& t = ctx.theme();
        auto r = getLocalBounds().toFloat();
        drawPlate(g, r, t, true, t.corner);
        g.setColour(t.border.withAlpha(0.6f));
        g.fillRect(juce::Rectangle<float>(r.getX() + 8.0f, static_cast<float>(tabsBottom) + 2.0f, r.getWidth() - 16.0f, 1.0f));
    }
    void resized() override {
        auto r = getLocalBounds().reduced(8);
        // Wrapping row of tab chips; the close button sits at the top right.
        const int rowH = 26, gap = 4;
        auto closeArea = r.removeFromTop(rowH);
        closeChip.setBounds(closeArea.removeFromRight(30));
        int x = closeArea.getX();
        int rowRight = closeArea.getRight();
        int cy = closeArea.getY();
        for (auto& tab : tabs) {
            const int w = std::max(46, tab->preferredWidth());
            if (x + w > rowRight) {
                x = r.getX();
                cy += rowH + gap;
                rowRight = r.getRight();
            }
            tab->setBounds(x, cy, w, rowH);
            x += w + gap;
        }
        tabsBottom = cy + rowH + 2;
        auto content = getLocalBounds().reduced(8);
        content.removeFromTop(std::max(0, tabsBottom + 6 - content.getY()));
        for (auto& p : panels)
            if (p) p->setBounds(content);
    }
    ShellContext& ctx;
    EditorBridge& bridge;
    PanelId current = PanelId::Presets;
    std::vector<std::unique_ptr<ChipButton>> tabs;
    ChipButton closeChip;
    std::array<std::unique_ptr<Panel>, kPanelCount> panels;
    int tabsBottom = 0;
};

juce::String factorText(int f) { return f <= 1 ? juce::String("Off") : juce::String(f) + "x"; }

bool visibleChain(const juce::Component* c, const juce::Component* stopAt) {
    for (; c != nullptr && c != stopAt; c = c->getParentComponent())
        if (!c->isVisible()) return false;
    return c == stopAt;
}

std::vector<std::pair<const juce::Component*, EditorCore*>>& registry() {
    static std::vector<std::pair<const juce::Component*, EditorCore*>> r;
    return r;
}

}  // namespace

// =============================================================================================
// EditorCore::Parts
// =============================================================================================
struct EditorCore::Parts : private juce::Timer, private juce::ComponentListener {
    Parts(EditorCore& o, juce::Component& h, EditorBridge& b, juce::ComponentBoundsConstrainer& c)
        : owner(o), host(h), bridge(b), constrainer(c) {
        ctx.ui = bridge.uiSettings();
        ctx.ui.scale = std::clamp(ctx.ui.scale, kMinScale, kMaxScale);
        ctx.ui.lastWidth = std::max(ctx.ui.lastWidth, kMinWidth);
        ctx.ui.lastHeight = std::max(ctx.ui.lastHeight, kMinHeight);
        lite = bridge.status().lite;
        persisted = ctx.ui;
        laf.apply(ctx.theme());

        root = std::make_unique<ShellRoot>(ctx);
        root->setLookAndFeel(&laf);
        host.addAndMakeVisible(*root);
        header = std::make_unique<HeaderBar>(ctx, lite);
        status = std::make_unique<StatusBar>(ctx);
        easy = std::make_unique<EasyView>(ctx, lite);
        macros = std::make_unique<MacroRow>(ctx, bridge, lite);
        util = std::make_unique<UtilityStrip>(ctx, bridge, lite);
        drawer = std::make_unique<Drawer>(ctx, bridge);
        root->addAndMakeVisible(*header);
        root->addAndMakeVisible(*status);
        root->addAndMakeVisible(*easy);
        root->addAndMakeVisible(*macros);
        root->addAndMakeVisible(*util);
        root->addChildComponent(*drawer);
        help = std::make_unique<HelpOverlay>(ctx, *root);
        root->addAndMakeVisible(*help);

        header->setExplicitFocusOrder(1);
        easy->setExplicitFocusOrder(2);
        macros->setExplicitFocusOrder(3);
        util->setExplicitFocusOrder(4);
        drawer->setExplicitFocusOrder(5);

        wire();
        root->onKey = [this](const juce::KeyPress& k) { return handleKey(k); };
        lastNewMs = juce::Time::getMillisecondCounter();
        constrainer.setMinimumSize(juce::roundToInt(kMinWidth * ctx.ui.scale), juce::roundToInt(kMinHeight * ctx.ui.scale));
        constrainer.setMaximumSize(8192, 8192);
        applyView();
        host.addComponentListener(this);
    }

    ~Parts() override {
        host.removeComponentListener(this);
        stopTimer();
        // The editor owns frame production. Finalize before its visual surfaces disappear;
        // the bridge's worker owns any already submitted images and has no borrowed renderer.
        bool finalizeCapture = true;
        try {
            const auto captureState = bridge.captureStatus().state;
            finalizeCapture = captureState == CaptureState::Armed || captureState == CaptureState::Recording;
        } catch (...) {
            // Status diagnostics can allocate. If the query fails, still make one
            // best-effort stop request without allowing an exception out of teardown.
        }
        if (finalizeCapture) {
            try { bridge.stopCapture(); } catch (...) {}
        }
        help.reset();
        if (root) root->setLookAndFeel(nullptr);
    }

    // ---- wiring ----
    void wire() {
        header->aChip.onClick = [this] { bridge.setEditSlot(0); };
        header->bChip.onClick = [this] { bridge.setEditSlot(1); };
        header->copyChip.onClick = [this] { bridge.copySlot(editSlot, 1 - editSlot); };
        header->swapChip.onClick = [this] { bridge.swapSlots(); };
        header->undoChip.onClick = [this] { bridge.undo(); };
        header->redoChip.onClick = [this] { bridge.redo(); };
        header->easyChip.onClick = [this] { owner.setView(EditorView::Easy); };
        header->workbenchChip.onClick = [this] { owner.setView(EditorView::Workbench); };
        header->panelsChip.onClick = [this] { owner.setDrawerOpen(!drawerOpen); };
        header->presetChip.onClick = [this] { owner.selectPanel(PanelId::Presets); };
        header->gearChip.onClick = [this] { owner.showSettingsMenu(); };
        status->onQualityClick = [this] { owner.selectPanel(PanelId::Quality); };
        drawer->closeChip.onClick = [this] { owner.setDrawerOpen(false); };
        for (int i = 0; i < static_cast<int>(easy->scenes.size()); ++i)
            easy->scenes[static_cast<size_t>(i)]->onClick = [this, i] { owner.applyScene(i); };
    }

    bool handleKey(const juce::KeyPress& k) {
        if (k == juce::KeyPress::escapeKey) {
            if (help->isOpen()) { help->dismiss(); return true; }
            if (drawerOpen && narrow) { owner.setDrawerOpen(false); return true; }
            return false;
        }
        const auto m = k.getModifiers();
        if ((m.isCommandDown() || m.isCtrlDown()) && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z')) {
            if (m.isShiftDown()) bridge.redo(); else bridge.undo();
            return true;
        }
        if ((m.isCommandDown() || m.isCtrlDown()) && (k.getKeyCode() == 'Y' || k.getKeyCode() == 'y')) { bridge.redo(); return true; }
        return false;
    }

    // ---- scale / layout ----
    double effectiveScale() const {
        const double pw = std::max(1, host.getWidth()), ph = std::max(1, host.getHeight());
        return std::min({static_cast<double>(ctx.ui.scale), pw / kMinWidth, ph / kMinHeight});
    }

    void hostResized() {
        // A resize can arrive before the next timer tick after a host-state load.
        // Apply its other preferences first, but keep this newer explicit host size.
        if (!applyingSettings) syncSettings(false);
        eff = effectiveScale();
        const int lw = juce::roundToInt(host.getWidth() / eff), lh = juce::roundToInt(host.getHeight() / eff);
        logical = {0, 0, std::max(lw, 1), std::max(lh, 1)};
        root->setTransform(juce::AffineTransform::scale(static_cast<float>(eff)));
        root->setBounds(logical);
        root->rebuildBackground(static_cast<float>(eff) * 1.0f);
        layout();
        if (applyingSettings) return;  // restoring preferences must not write them back
        // Remember the logical window size (user scale) for the next session.
        const int uw = juce::roundToInt(host.getWidth() / static_cast<double>(ctx.ui.scale));
        const int uh = juce::roundToInt(host.getHeight() / static_cast<double>(ctx.ui.scale));
        if (uw != ctx.ui.lastWidth || uh != ctx.ui.lastHeight) {
            ctx.ui.lastWidth = std::max(uw, kMinWidth);
            ctx.ui.lastHeight = std::max(uh, kMinHeight);
            persist();
        }
    }

    void layout() {
        const int W = logical.getWidth(), H = logical.getHeight();
        narrow = W < kNarrowBelow;
        header->setBounds(0, 0, W, kHeaderH);
        header->layout(narrow);
        status->setBounds(0, kHeaderH, W, kStatusH);
        const int uh = lite ? 64 : 84;
        util->setBounds(0, H - uh, W, uh);
        juce::Rectangle<int> main(0, kHeaderH + kStatusH, W, std::max(0, H - kHeaderH - kStatusH - uh));
        const bool showDrawer = drawerOpen && !lite;
        juce::Rectangle<int> drawerBounds;
        if (showDrawer && !narrow) {
            const int dw = std::clamp(static_cast<int>(W * 0.34), 300, 400);
            drawerBounds = main.removeFromRight(dw).reduced(0, 4).withTrimmedRight(kMargin).withTrimmedLeft(2);
        }
        const bool easyView = lite || ctx.ui.view == EditorView::Easy;
        const int mh = easyView ? std::clamp(static_cast<int>(main.getHeight() * 0.30), 104, 190) : 78;
        auto macroArea = main.removeFromBottom(mh);
        macros->setBounds(macroArea.reduced(kMargin, 4));
        auto body = main.reduced(kMargin, 4);
        if (showDrawer && narrow) drawerBounds = body;
        drawer->setBounds(drawerBounds);
        drawer->setVisible(showDrawer);
        const bool coveredByDrawer = showDrawer && narrow;
        easy->setBounds(body);
        easy->setVisible(easyView && !coveredByDrawer);
        if (workbench) {
            workbench->setBounds(body);
            workbench->setVisible(!easyView && !coveredByDrawer);
        }
        help->setBounds(root->getLocalBounds());
        help->toFront(false);
        // Header toggles.
        header->easyChip.setToggleState(easyView, juce::dontSendNotification);
        header->workbenchChip.setToggleState(!easyView, juce::dontSendNotification);
        header->panelsChip.setToggleState(drawerOpen, juce::dontSendNotification);
        root->repaint();
    }

    void applyView() {
        const bool wantWorkbench = !lite && ctx.ui.view == EditorView::Workbench;
        if (wantWorkbench && !workbench) {
            workbench = std::make_unique<WorkbenchView>(bridge);
            root->addChildComponent(*workbench);
            workbench->setTitle("Workbench view");
            workbench->setExplicitFocusOrder(2);
            workbench->settingsChanged(ctx.ui);
        }
        layout();
    }

    // ---- settings persistence ----
    void syncSettings(bool resizeToSaved = true) {
        if (applyingSettings) return;
        const auto current = bridge.uiSettings();
        if (sameSettings(current, persisted)) return;
        const juce::ScopedValueSetter<bool> guard(applyingSettings, true);
        ctx.ui = current;
        persisted = current;
        laf.apply(ctx.theme());
        constrainer.setMinimumSize(juce::roundToInt(kMinWidth * ctx.ui.scale),
                                   juce::roundToInt(kMinHeight * ctx.ui.scale));
        applyView();
        if (resizeToSaved)
            host.setSize(juce::roundToInt(ctx.ui.lastWidth * ctx.ui.scale),
                         juce::roundToInt(ctx.ui.lastHeight * ctx.ui.scale));
        hostResized();
        broadcastSettings();
    }

    void persist() {
        auto cur = bridge.uiSettings();  // workbench-owned fields (overlay, xray) must not be clobbered
        cur.scale = ctx.ui.scale;
        cur.darkTheme = ctx.ui.darkTheme;
        cur.view = ctx.ui.view;
        cur.educational = ctx.ui.educational;
        cur.reduceMotion = ctx.ui.reduceMotion;
        cur.lowFrameRate = ctx.ui.lowFrameRate;
        cur.lastWidth = ctx.ui.lastWidth;
        cur.lastHeight = ctx.ui.lastHeight;
        ctx.ui.overlay = cur.overlay;
        ctx.ui.xray = cur.xray;
        ctx.ui.pcbView = cur.pcbView;
        bridge.setUiSettings(cur);
        persisted = cur;
    }

    void broadcastSettings() {
        if (workbench) workbench->settingsChanged(ctx.ui);
        for (auto& p : drawer->panels)
            if (p) p->settingsChanged(ctx.ui);
        help->refresh();
        root->repaint();
        restartTimer();
    }

    // ---- timer ----
    void restartTimer() {
        if (active) { hz = ctx.ui.lowFrameRate ? 30 : 60; startTimerHz(hz); }
    }
    void setActive(bool a) {
        if (a) syncSettings();
        active = a;
        idle = false;
        if (active) {
            lastTickMs = juce::Time::getMillisecondCounterHiRes();
            restartTimer();
        } else {
            stopTimer();
            help->dismiss();
            // No painting while hidden. The capture worker keeps its own deadline and repeats
            // the last admitted frame for remaining ticks, so closing a host window cannot
            // leave a recording waiting for another visible drawer refresh.
        }
    }
    void componentVisibilityChanged(juce::Component&) override { setActive(host.isShowing()); }
    void componentParentHierarchyChanged(juce::Component&) override { setActive(host.isShowing()); }
    void timerCallback() override {
        if (!host.isShowing()) {  // minimized / hidden: no telemetry reads, just a slow watcher
            if (!idle) { idle = true; startTimer(500); }
            return;
        }
        if (idle) { idle = false; restartTimer(); lastTickMs = juce::Time::getMillisecondCounterHiRes(); return; }
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double dt = std::clamp((now - lastTickMs) / 1000.0, 0.0, 0.25);
        lastTickMs = now;
        pumpFrame(dt, juce::Time::getMillisecondCounter());
    }
    bool timerRunning() const { return isTimerRunning(); }
    int timerHz() const { return isTimerRunning() && !idle ? hz : 0; }

    double macroValue(const char* id) const {
        if (auto* v = bridge.parameters().getRawParameterValue(id)) return static_cast<double>(v->load());
        return 0.0;
    }

    void pumpFrame(double dt, juce::uint32 nowMs) {
        syncSettings();
        engine::TelemetryFrame frame;  // read exactly once per UI frame
        const bool valid = bridge.readTelemetry(frame);
        if (valid && frame.generation != lastGeneration) {
            lastGeneration = frame.generation;
            lastNewMs = nowMs;
        }
        stale = !valid || (nowMs - lastNewMs) > static_cast<juce::uint32>(kStaleMs);
        const StatusInfo st = bridge.status();
        editSlot = st.editSlot;
        lastStatus = st;
        updateHeader(st);
        updateStatusBar(st);

        SphereInputs si;
        si.valid = valid && !stale;
        if (valid) {
            const auto& slot = frame.slot[std::clamp(st.editSlot, 0, engine::kSlots - 1)];
            si.railVolts = slot.railVolts;
            si.railNominalVolts = slot.railNominalVolts;
            si.outRmsLinear = std::max(frame.outRms[0], frame.outRms[1]);
        }
        if (lite) {
            si.sag = macroValue("lite.sag");
            si.age = macroValue("lite.thermalAge");
            si.drift = 0.25 * si.age;
            si.warmth = 0.2;
        } else {
            si.sag = macroValue("macro.sag");
            si.age = macroValue("macro.age");
            si.drift = macroValue("macro.drift");
            si.warmth = macroValue("macro.warmth");
        }
        si.reduceMotion = ctx.ui.reduceMotion;
        easy->sphere.update(si, dt);
        for (int i = 0; i < static_cast<int>(easy->scenes.size()); ++i)
            easy->scenes[static_cast<size_t>(i)]->setToggleState(owner.sceneMatches(i), juce::dontSendNotification);
        // A narrow drawer covers the selected view, but it remains the capture source.
        if (workbench && !lite && ctx.ui.view == EditorView::Workbench)
            workbench->updateFrame(frame, valid && !stale);
        pumpCaptureFrame();
        if (drawer->isVisible())
            if (auto* p = drawer->currentPanel()) p->refresh(st);
    }

    juce::Image captureSnapshot(CaptureFormat format, CaptureFraming framing) {
        const auto size = captureDimensions(format, framing);
        if (size.width <= 0 || size.height <= 0) return {};
        auto* surface = !lite && ctx.ui.view == EditorView::Workbench
                            ? static_cast<juce::Component*>(workbench.get())
                            : static_cast<juce::Component*>(easy.get());
        if (surface == nullptr || surface->getWidth() <= 0 || surface->getHeight() <= 0) return {};
        try {
            // Allocate only the shared output size, directly in CPU memory. Rendering a scaled
            // host snapshot and then resampling would retain an extra, potentially large image
            // and can silently select a native/GPU image type on some hosts.
            juce::Image image(juce::Image::ARGB, size.width, size.height, true, juce::SoftwareImageType());
            if (!image.isValid()) return {};
            juce::Graphics graphics(image);
            graphics.fillAll(ctx.theme().background);
            const float scale = std::max(static_cast<float>(size.width) / surface->getWidth(),
                                         static_cast<float>(size.height) / surface->getHeight());
            const float x = (size.width - surface->getWidth() * scale) * 0.5f;
            const float y = (size.height - surface->getHeight() * scale) * 0.5f;
            graphics.addTransform(juce::AffineTransform::scale(scale).translated(x, y));
            // paintEntireComponent paints this explicitly owned subtree even when the drawer
            // has hidden it. It never visits a parent, the editor header, or a host sibling.
            surface->paintEntireComponent(graphics, true);
            return image;
        } catch (...) {
            // The recording backend treats an invalid submitted image as a terminal failure;
            // still-image actions show their own render failure without escaping the UI callback.
            return {};
        }
    }

    void pumpCaptureFrame() {
        const auto request = bridge.takeCaptureFrameRequest();
        if (!request) return;
        const auto size = captureDimensions(CaptureFormat::Gif, request->framing);
        // One current frame per UI opportunity; the worker fills missed absolute clock ticks.
        // Do not allocate from request dimensions independently of the shared admitted sizes.
        auto image = size.width == request->width && size.height == request->height
                         ? captureSnapshot(CaptureFormat::Gif, request->framing) : juce::Image{};
        bridge.submitCaptureFrame(*request, image);
    }

    void updateHeader(const StatusInfo& st) {
        const auto h = bridge.history();
        auto& hb = *header;
        const juce::String name = st.presetName.isEmpty() ? juce::String("Untitled") : st.presetName;
        presetText = name + (st.presetModified ? "  (Modified)" : "");
        if (hb.presetChip.getButtonText() != presetText) hb.presetChip.setText(presetText);
        hb.presetChip.setTitle("Preset " + presetText);
        hb.aChip.setToggleState(st.editSlot == 0, juce::dontSendNotification);
        hb.bChip.setToggleState(st.editSlot == 1, juce::dontSendNotification);
        const juce::String from = st.editSlot == 0 ? "A" : "B", to = st.editSlot == 0 ? "B" : "A";
        hb.copyChip.setTitle("Copy slot " + from + " to " + to);
        hb.undoChip.setEnabled(h.canUndo);
        hb.redoChip.setEnabled(h.canRedo);
        const juce::String ul = h.canUndo ? "Undo " + h.undoLabel : "Undo (nothing to undo)";
        const juce::String rl = h.canRedo ? "Redo " + h.redoLabel : "Redo (nothing to redo)";
        hb.undoChip.setTitle(ul);
        hb.redoChip.setTitle(rl);
        setTip(hb.undoChip, ul, h.canUndo ? "Undo the last change (" + h.undoLabel + ")." : "Nothing to undo.");
        setTip(hb.redoChip, rl, h.canRedo ? "Redo the change (" + h.redoLabel + ")." : "Nothing to redo.");
        if (auto* bp = util->bypass.get(); bp != nullptr && bp->isIndicator()) bp->setIndicator(st.bypassed, "ACTIVE", "OFF");
    }

    void updateStatusBar(const StatusInfo& st) {
        const bool zero = st.zeroLatency;
        qualityText = "Quality " + factorText(st.activeOversampling) + (st.activeIir ? " IIR" : " FIR");
        if (st.reprepareRequired || st.pendingOversampling != st.activeOversampling || st.pendingIir != st.activeIir)
            qualityText << " > " << factorText(st.pendingOversampling) << (st.pendingIir ? " IIR" : " FIR") << " pending";
        if (st.draftQuality) qualityText << " | DRAFT";
        if (zero) qualityText << " | Zero-Latency 0 smp";
        else {
            qualityText << " | " << st.latencySamples << " smp";
            if (st.sampleRate > 0) qualityText << " / " << juce::String(1000.0 * st.latencySamples / st.sampleRate, 1) << " ms";
        }
        std::vector<StatusBar::Badge> b;
        if (st.containmentActive) b.push_back({"Containment", StatusBar::Kind::Danger});
        if (st.solverFallback) b.push_back({"Solver fallback", StatusBar::Kind::Warn});
        if (st.needsReset) b.push_back({"Reset needed", StatusBar::Kind::Warn});
        if (st.bypassed) b.push_back({"Bypassed", StatusBar::Kind::Warn});
        if (st.tempoFallback) b.push_back({"Tempo fallback " + juce::String(juce::roundToInt(st.bpm)) + " BPM", StatusBar::Kind::Info});
        if (stale) b.push_back({"Stale", StatusBar::Kind::Info});
        badgeTexts.clear();
        for (auto& x : b) badgeTexts.add(x.text);
        status->set(qualityText, std::move(b));
    }

    EditorCore& owner;
    juce::Component& host;
    EditorBridge& bridge;
    juce::ComponentBoundsConstrainer& constrainer;
    ShellLookAndFeel laf;
    ShellContext ctx;
    UiSettings persisted;
    bool lite = false, narrow = false, drawerOpen = false, active = false, stale = false, idle = false;
    bool applyingSettings = false;
    PanelId currentPanelId = PanelId::Presets;
    double eff = 1.0, lastTickMs = 0;
    int hz = 60;
    juce::Rectangle<int> logical{0, 0, kBaseWidth, kBaseHeight};
    std::uint64_t lastGeneration = 0;
    juce::uint32 lastNewMs = 0;
    int editSlot = 0;
    StatusInfo lastStatus;
    juce::String qualityText, presetText;
    juce::StringArray badgeTexts;

    std::unique_ptr<ShellRoot> root;
    std::unique_ptr<HeaderBar> header;
    std::unique_ptr<StatusBar> status;
    std::unique_ptr<EasyView> easy;
    std::unique_ptr<MacroRow> macros;
    std::unique_ptr<UtilityStrip> util;
    std::unique_ptr<Drawer> drawer;
    std::unique_ptr<WorkbenchView> workbench;
    std::unique_ptr<HelpOverlay> help;
};

// =============================================================================================
// EditorCore
// =============================================================================================
EditorCore::EditorCore(juce::Component& host, EditorBridge& bridge, juce::ComponentBoundsConstrainer& constrainer)
    : p_(std::make_unique<Parts>(*this, host, bridge, constrainer)) {
    p_->hostResized();
    p_->pumpFrame(0.0, juce::Time::getMillisecondCounter());  // prime so the first paint shows real state
    registry().push_back({&host, this});
}

EditorCore::~EditorCore() {
    auto& r = registry();
    r.erase(std::remove_if(r.begin(), r.end(), [this](auto& e) { return e.second == this; }), r.end());
}

EditorCore* EditorCore::find(const juce::Component* host) {
    for (auto& e : registry())
        if (e.first == host) return e.second;
    return nullptr;
}

void EditorCore::hostResized() { p_->hostResized(); }
void EditorCore::setActive(bool visible) { p_->setActive(visible); }
void EditorCore::pumpFrame(double dt, juce::uint32 nowMs) { p_->pumpFrame(dt, nowMs); }
juce::Image EditorCore::captureSnapshot(CaptureFormat format, CaptureFraming framing) {
    p_->syncSettings();
    return p_->captureSnapshot(format, framing);
}
ShellContext& EditorCore::context() noexcept { return p_->ctx; }
HelpOverlay& EditorCore::help() noexcept { return *p_->help; }
ToneSphere& EditorCore::sphere() noexcept { return p_->easy->sphere; }
bool EditorCore::isLite() const noexcept { return p_->lite; }
bool EditorCore::isNarrow() const noexcept { return p_->narrow; }
double EditorCore::effectiveScale() const noexcept { return p_->eff; }
juce::Rectangle<int> EditorCore::logicalBounds() const noexcept { return p_->logical; }
int EditorCore::refreshRateHz() const noexcept { return p_->timerHz(); }
bool EditorCore::timerRunning() const noexcept { return p_->timerRunning(); }
bool EditorCore::drawerOpen() const noexcept { return p_->drawerOpen; }
PanelId EditorCore::currentPanel() const noexcept { return p_->drawer->current; }
EditorView EditorCore::view() const noexcept { return p_->ctx.ui.view; }
int EditorCore::sceneCount() noexcept { return static_cast<int>(std::size(kScenes)); }
const char* EditorCore::sceneName(int i) noexcept { return kScenes[std::clamp(i, 0, sceneCount() - 1)].name; }
bool EditorCore::staleBadgeShown() const noexcept { return p_->stale; }
juce::String EditorCore::qualityBadgeText() const { return p_->qualityText; }
juce::StringArray EditorCore::statusBadgeTexts() const { return p_->badgeTexts; }
juce::String EditorCore::presetLabelText() const { return p_->presetText; }

void EditorCore::setView(EditorView v) {
    p_->syncSettings();
    if (p_->lite) return;
    p_->ctx.ui.view = v;
    p_->applyView();
    p_->persist();
    p_->broadcastSettings();
}

void EditorCore::setDarkTheme(bool dark) {
    p_->syncSettings();
    p_->ctx.ui.darkTheme = dark;
    p_->laf.apply(p_->ctx.theme());
    p_->root->rebuildBackground(static_cast<float>(p_->eff));
    p_->persist();
    p_->broadcastSettings();
}

void EditorCore::setUserScale(float s) {
    p_->syncSettings();
    s = std::clamp(s, kMinScale, kMaxScale);
    auto& ui = p_->ctx.ui;
    // Keep the logical size; the host window follows (and may refuse, in which case the content
    // reflows at a smaller effective scale instead of being cropped).
    const int lw = std::max(kMinWidth, ui.lastWidth), lh = std::max(kMinHeight, ui.lastHeight);
    ui.scale = s;
    p_->constrainer.setMinimumSize(juce::roundToInt(kMinWidth * s), juce::roundToInt(kMinHeight * s));
    p_->host.setSize(juce::roundToInt(lw * s), juce::roundToInt(lh * s));
    p_->hostResized();
    p_->persist();
    p_->broadcastSettings();
}

void EditorCore::setEducational(bool on) { p_->syncSettings(); p_->ctx.ui.educational = on; p_->persist(); p_->broadcastSettings(); }
void EditorCore::setReduceMotion(bool on) { p_->syncSettings(); p_->ctx.ui.reduceMotion = on; p_->persist(); p_->broadcastSettings(); }
void EditorCore::setLowFrameRate(bool on) { p_->syncSettings(); p_->ctx.ui.lowFrameRate = on; p_->persist(); p_->broadcastSettings(); }

void EditorCore::setDrawerOpen(bool open) {
    if (p_->lite) return;
    p_->drawerOpen = open;
    p_->layout();
    if (!open) p_->help->dismiss();
}

void EditorCore::selectPanel(PanelId id) {
    if (p_->lite) return;
    p_->drawer->select(id);
    setDrawerOpen(true);
    if (auto* pn = p_->drawer->currentPanel()) pn->refresh(p_->lastStatus);
}

void EditorCore::applyScene(int index) {
    if (index < 0 || index >= sceneCount()) return;
    const auto& s = kScenes[index];
    auto& apvts = p_->bridge.parameters();
    auto set = [&](const char* id, double v) {
        if (auto* prm = apvts.getParameter(id)) {
            p_->bridge.touchParameter(id);
            prm->beginChangeGesture();
            prm->setValueNotifyingHost(prm->convertTo0to1(static_cast<float>(v)));
            prm->endChangeGesture();
        }
    };
    p_->bridge.beginGesture(juce::String("Scene: ") + s.name);  // one transaction
    set("macro.age", s.age);
    set("macro.sag", s.sag);
    set("macro.drift", s.drift);
    set("macro.warmth", s.warmth);
    set("analogNoise.enabled", s.noise ? 1.0 : 0.0);
    set("mix", 1.0);
    set("gainMatch.enabled", 1.0);
    p_->bridge.endGesture();
}

bool EditorCore::sceneMatches(int index) const {
    if (index < 0 || index >= sceneCount()) return false;
    const auto& s = kScenes[index];
    auto near = [](double a, double b) { return std::abs(a - b) < 0.005; };
    return near(p_->macroValue("macro.age"), s.age) && near(p_->macroValue("macro.sag"), s.sag) &&
           near(p_->macroValue("macro.drift"), s.drift) && near(p_->macroValue("macro.warmth"), s.warmth) &&
           near(p_->macroValue("analogNoise.enabled"), s.noise ? 1.0 : 0.0);
}

void EditorCore::showSettingsMenu() {
    p_->syncSettings();
    juce::PopupMenu m, scale;
    const auto& ui = p_->ctx.ui;
    m.addSectionHeader("Theme");
    m.addItem(10, "Dark brushed metal", true, ui.darkTheme);
    m.addItem(11, "Light", true, !ui.darkTheme);
    for (int pct : {75, 100, 125, 150, 175, 200})
        scale.addItem(100 + pct, juce::String(pct) + " %", true, std::abs(ui.scale * 100.0f - static_cast<float>(pct)) < 1.0f);
    m.addSubMenu("Scale", scale);
    m.addSeparator();
    m.addItem(20, "Educational tooltips (long text)", true, ui.educational);
    m.addItem(21, "Keep tooltips open (pin)", true, p_->ctx.pinTooltips);
    m.addItem(22, "Reduce motion", true, ui.reduceMotion);
    m.addItem(23, "Low frame rate (30 Hz)", true, ui.lowFrameRate);
    juce::Component::SafePointer<juce::Component> target(&p_->header->gearChip);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target.getComponent()),
                    [this, target](int r) {
                        // The menu callback can arrive after the editor has closed. The target is
                        // owned by this core, so its weak pointer must be checked before using this.
                        if (target == nullptr) return;
                        if (r != 0) p_->syncSettings();
                        if (r == 10) setDarkTheme(true);
                        else if (r == 11) setDarkTheme(false);
                        else if (r >= 175 && r <= 300) setUserScale(static_cast<float>(r - 100) / 100.0f);
                        else if (r == 20) setEducational(!p_->ctx.ui.educational);
                        else if (r == 21) { p_->ctx.pinTooltips = !p_->ctx.pinTooltips; }
                        else if (r == 22) setReduceMotion(!p_->ctx.ui.reduceMotion);
                        else if (r == 23) setLowFrameRate(!p_->ctx.ui.lowFrameRate);
                    });
}

std::vector<EditorCore::Item> EditorCore::essentialControls() const {
    std::vector<Item> out;
    auto* stop = p_->host.isParentOf(p_->root.get()) ? &p_->host : nullptr;
    auto add = [&](juce::Component* c) {
        if (c != nullptr && c->getWidth() > 0 && c->getHeight() > 0 && visibleChain(c, stop))
            out.push_back({c->getTitle().isNotEmpty() ? c->getTitle() : c->getName(), c});
    };
    for (auto* c : p_->header->chips()) add(c);
    add(&p_->header->gearChip);
    for (auto& s : p_->easy->scenes) add(s.get());
    for (auto& k : p_->macros->knobs) add(k.get());
    for (auto& k : p_->util->knobs) add(k.get());
    for (auto& s : p_->util->switches) add(s.get());
    add(p_->util->bypass.get());
    // Header chips list already includes the gear chip; drop duplicates by pointer.
    std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.comp < b.comp; });
    out.erase(std::unique(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.comp == b.comp; }), out.end());
    return out;
}

std::vector<juce::Component*> EditorCore::allKnobs() const {
    std::vector<juce::Component*> k;
    for (auto& x : p_->macros->knobs) k.push_back(x.get());
    for (auto& x : p_->util->knobs) k.push_back(x.get());
    return k;
}

}  // namespace cd::plugin::ui::shell
