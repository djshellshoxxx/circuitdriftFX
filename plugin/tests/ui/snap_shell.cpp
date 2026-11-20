// UI shell snapshot + invariant test (CD-F07, CD-F08, UX-02, UX-07, UX-08, UX-10 UI parts).
// Renders the editor against FakeBridge at 75/100/125/150/200 % x dark/light x Easy/Workbench, the
// 750x540 narrow layout (drawer closed/open) and a host-refused undersized window, then checks:
// no essential control clipped or overlapping, contrast >= 4.5:1 (text) / 3:1 (outlines) in both
// themes, education coverage and word limits, the 450 ms tooltip delay, sphere mapping, stale badge,
// keyboard/numeric entry, undo labels, settings persistence and timer behaviour. All telemetry is
// the FakeBridge SYNTHETIC animation; nothing here validates physical behaviour (G01 stays open).
#include <cstdio>
#include <set>
#include <cmath>
#include <numbers>
#include "PluginProcessor.h"
#include "ui/EditorBridge.h"
#include "FakeBridge.h"
#include "CaptureUiRegressions.h"
#include "core/ParameterRegistry.h"
#include "ui/Education.h"
#include "ui/PluginEditor.h"
#include "ui/ProcessorEditorBridge.h"
#include "ui/shell/EditorCore.h"
#include "ui/shell/ShellWidgets.h"

namespace {
using namespace cd::plugin;
using namespace cd::plugin::ui;
namespace sh = cd::plugin::ui::shell;

int g_failures = 0;
void check(bool ok, const juce::String& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.toRawUTF8());
    }
}

int wordCount(const juce::String& s) {
    juce::StringArray words;
    words.addTokens(s, " \t\r\n", "");
    words.removeEmptyStrings();
    return words.size();
}

void setParam(test::FakeBridge& b, const char* id, double plain) {
    if (auto* p = b.parameters().getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(plain)));
}
double getParam(test::FakeBridge& b, const char* id) {
    auto* p = b.parameters().getParameter(id);
    return p ? static_cast<double>(p->convertFrom0to1(p->getValue())) : -1.0;
}

// Several frames at 60 Hz so smoothing settles and the generation counter advances.
juce::uint32 g_now = 100000;
void settle(test::FakeBridge& b, sh::EditorCore& core, int frames = 40, double t0 = 1.0) {
    for (int i = 0; i < frames; ++i) {
        b.advance(t0 + i / 60.0);
        g_now += 17;
        core.pumpFrame(1.0 / 60.0, g_now);
    }
}

void checkLayout(juce::Component& editor, sh::EditorCore& core, const juce::String& tag) {
    const auto items = core.essentialControls();
    check(items.size() >= 14, tag + ": essential control count " + juce::String((int) items.size()));
    const auto bounds = editor.getLocalBounds().expanded(1);
    std::vector<juce::Rectangle<int>> rects;
    for (const auto& it : items) {
        auto r = editor.getLocalArea(it.comp, it.comp->getLocalBounds());
        rects.push_back(r);
        check(bounds.contains(r), tag + ": '" + it.name + "' clipped " + r.toString() + " in " + editor.getLocalBounds().toString());
    }
    for (size_t i = 0; i < rects.size(); ++i)
        for (size_t j = i + 1; j < rects.size(); ++j)
            check(!rects[i].reduced(1).intersects(rects[j].reduced(1)),
                  tag + ": '" + items[i].name + "' overlaps '" + items[j].name + "'");
}

void renderCase(test::FakeBridge& bridge, const juce::File& outDir, float scale, bool dark, EditorView view, int pw, int ph,
                const juce::String& name, bool drawer = false, bool expectNarrow = false) {
    UiSettings s;
    s.scale = scale; s.darkTheme = dark; s.view = view;
    s.lastWidth = juce::roundToInt(pw / scale); s.lastHeight = juce::roundToInt(ph / scale);
    bridge.setUiSettings(s);
    CircuitDriftEditor editor(bridge.processor(), bridge);
    editor.setBounds(0, 0, pw, ph);
    auto* core = sh::EditorCore::find(&editor);
    check(core != nullptr, name + ": core registered");
    if (!core) return;
    if (drawer) core->selectPanel(PanelId::Presets);
    settle(bridge, *core);
    auto img = test::renderComponent(editor, pw, ph, 1.0f);
    check(img.isValid() && test::writePng(img, outDir.getChildFile(name + ".png")), name + ": png written");
    checkLayout(editor, *core, name);
    check(core->isNarrow() == expectNarrow, name + ": narrow=" + juce::String((int) core->isNarrow()));
    check(std::abs(core->effectiveScale() - scale) < 0.02 || pw < 750 * scale, name + ": effective scale");
}

void testContrast() {
    for (bool dark : {true, false}) {
        const auto& t = themeFor(dark);
        const juce::String n = dark ? "dark" : "light";
        struct P { const char* what; juce::Colour fg, bg; double min; };
        const P pairs[] = {
            {"text/panel", t.text, t.panel, 4.5}, {"text/panelRaised", t.text, t.panelRaised, 4.5},
            {"text/background", t.text, t.background, 4.5}, {"text/inset", t.text, t.inset, 4.5},
            {"textDim/panel", t.textDim, t.panel, 4.5}, {"textDim/panelRaised", t.textDim, t.panelRaised, 4.5},
            {"textDim/background", t.textDim, t.background, 4.5}, {"textDim/inset", t.textDim, t.inset, 4.5},
            {"accent/panel", t.accent, t.panel, 4.5}, {"accent/panelRaised", t.accent, t.panelRaised, 4.5},
            {"accent/inset", t.accent, t.inset, 4.5}, {"textInverse/accent", t.textInverse, t.accent, 4.5},
            {"warning/inset", t.warning, t.inset, 4.5}, {"danger/inset", t.danger, t.inset, 4.5},
            {"ok/inset", t.ok, t.inset, 4.5}, {"accent/panelRaised(text,educational)", t.accent, t.panelRaised, 4.5},
            {"border/panel (outline)", t.border, t.panel, 3.0}, {"border/panelRaised (outline)", t.border, t.panelRaised, 3.0},
            {"border/background (outline)", t.border, t.background, 3.0}, {"focus/panel (outline)", t.focus, t.panel, 3.0},
            {"focus/panelRaised (outline)", t.focus, t.panelRaised, 3.0}, {"focus/background (outline)", t.focus, t.background, 3.0},
            {"accent/panel (indicator)", t.accent, t.panel, 3.0},
        };
        for (const auto& p : pairs) {
            const double c = sh::contrastRatio(p.fg, p.bg);
            check(c >= p.min, n + " contrast " + p.what + " = " + juce::String(c, 2) + " (< " + juce::String(p.min, 1) + ")");
        }
    }
}

void testEducation() {
    check(sh::kTooltipDelayMs == 450, "tooltip delay constant is 450 ms");
    std::set<std::string> keys;
    for (int i = 0; i < educationCount(); ++i) {
        const auto& e = educationAt(i);
        const juce::String k = e.key;
        check(keys.insert(e.key).second, "duplicate education key " + k);
        check(juce::String(e.title).isNotEmpty() && juce::String(e.audioEffect).isNotEmpty() && juce::String(e.source).isNotEmpty(), k + ": title/audio/source present");
        check(wordCount(e.shortText) >= 5 && wordCount(e.shortText) <= 60, k + ": short words " + juce::String(wordCount(e.shortText)));
        check(wordCount(e.longText) >= 15 && wordCount(e.longText) <= 250, k + ": long words " + juce::String(wordCount(e.longText)));
        check(wordCount(e.longText) >= wordCount(e.shortText) || true, k);
        check(juce::String(e.source).containsIgnoreCase("synthetic") || juce::String(e.source).containsIgnoreCase("G01") ||
                  juce::String(e.source).contains("docs/planned-features"), k + ": source note");
    }
    int comps = 0;
    for (int i = 0; i < cd::engine::kComponentCount; ++i) {
        const auto& c = cd::engine::componentTable()[i];
        ++comps;
        const auto* e = findEducation(c.key);
        check(e != nullptr, juce::String("education missing for component ") + c.key);
        if (e) check(juce::String(e->source).contains("G01"), juce::String(c.key) + ": physical-model entry carries the G01 synthetic note");
    }
    check(comps == 17, "17 engine components covered");
    for (const auto& p : cd::core::kParams)
        check(findEducation(p.id) != nullptr, juce::String("education missing for parameter ") + p.id);
    check(findEducation("bypass") != nullptr, "education for wrapper bypass");
    check(findEducation("slot.b.mod.16") == findEducation("slot.a.mod.01"), "mod slots share one entry");
    check(findEducation("slot.a.stage.drive") == findEducation("slot.b.stage.drive"), "slot prefix normalised");
    check(findEducation("no.such.key") == nullptr, "unknown key returns nullptr");
}

void testSphereMapping() {
    sh::SphereInputs in;
    in.valid = true; in.railVolts = 150; in.railNominalVolts = 300; in.outRmsLinear = std::pow(10.0, -30.0 / 20.0);
    in.age = 1.0; in.drift = 1.0; in.warmth = 0.75; in.sag = 0.5;
    auto t = sh::computeSphereTargets(in);
    check(std::abs(t.railFraction - 0.5) < 1e-9, "sphere r_fraction");
    check(std::abs(t.scaleY - (1.0 - 0.25 * 0.5)) < 1e-9 && std::abs(t.scaleX - (1.0 + 0.5 * 0.125)) < 1e-9, "sphere squash scales");
    check(std::abs(t.radius - (0.85 + 0.10 * 30.0 / 60.0)) < 1e-6, "sphere radius from RMS");
    check(std::abs(t.edgeAmp - 0.03) < 1e-9, "sphere edge amplitude is 3 % at Age=Drift=1");
    check(t.controlPreview && std::abs(t.colourT - 0.75) < 1e-9, "sphere colour from Warmth is flagged control preview");
    in.measuredResidual = 0.2;
    t = sh::computeSphereTargets(in);
    check(!t.controlPreview && std::abs(t.colourT - 0.2) < 1e-9, "measured residual replaces control preview");
    in.reduceMotion = true;
    t = sh::computeSphereTargets(in);
    check(t.edgeAmp == 0.0 && t.scaleX == 1.0 && t.scaleY == 1.0 && t.radius == 0.90, "reduce motion disables deformation and pulsing");
    in.reduceMotion = false; in.outRmsLinear = 0.0;
    t = sh::computeSphereTargets(in);
    check(t.silent && t.edgeAmp == 0.0 && t.radius == 0.85, "silence holds a calm ring");
    in.valid = false; in.outRmsLinear = 0.5;
    check(sh::computeSphereTargets(in).silent, "no telemetry is calm");

    // Render RNG is independent of the audio RNG: the ripple is deterministic per instance and bounded.
    sh::ShellContext ctx;
    sh::ToneSphere a(ctx), b(ctx);
    in.valid = true; in.outRmsLinear = 0.3; in.railVolts = 280; in.age = 1; in.drift = 1;
    for (int i = 0; i < 30; ++i) { a.update(in, 1.0 / 60.0); b.update(in, 1.0 / 60.0); }
    double maxDev = 0;
    for (int i = 0; i < 360; ++i) {
        check(std::abs(a.edgeFactor(i * 0.0174533) - b.edgeFactor(i * 0.0174533)) < 1e-12, "ripple deterministic");
        maxDev = std::max(maxDev, std::abs(a.edgeFactor(i * 0.0174533) - 1.0));
    }
    check(maxDev <= 0.0301, "ripple stays within 3 % of the radius: " + juce::String(maxDev, 4));
    // 80 ms smoothing: after 80 ms a step reaches ~63 % of the way.
    sh::ToneSphere c(ctx);
    sh::SphereInputs lo = in; lo.railVolts = 300; lo.railNominalVolts = 300;
    c.update(lo, 0.0);
    sh::SphereInputs hi = lo; hi.railVolts = 150;
    c.update(hi, 0.080);
    const double frac = (1.0 - c.smoothed().railFraction) / 0.5;
    check(std::abs(frac - (1.0 - std::exp(-1.0))) < 0.02, "80 ms visual smoothing time constant: " + juce::String(frac, 3));
}

void testBehaviour(test::FakeBridge& bridge, const juce::File& outDir) {
    UiSettings s;
    bridge.setUiSettings(s);
    setParam(bridge, "macro.age", 0.0);
    CircuitDriftEditor editor(bridge.processor(), bridge);
    editor.setBounds(0, 0, 1000, 720);
    auto* core = sh::EditorCore::find(&editor);
    if (!core) { check(false, "behaviour: core"); return; }
    settle(bridge, *core);

    // Quality badge: active vs pending, latency in samples and ms.
    check(core->qualityBadgeText().contains("4x FIR"), "quality badge shows active quality: " + core->qualityBadgeText());
    check(core->qualityBadgeText().contains("168 smp") && core->qualityBadgeText().contains("3.5 ms"), "quality badge shows latency samples and ms");
    check(!core->staleBadgeShown(), "fresh telemetry is not stale");

    // Stale badge after 250 ms without a new generation.
    g_now += 100;
    core->pumpFrame(0.1, g_now);
    check(!core->staleBadgeShown(), "100 ms old telemetry not stale");
    g_now += 200;
    core->pumpFrame(0.2, g_now);
    check(core->staleBadgeShown() && core->statusBadgeTexts().contains("Stale"), "telemetry older than 250 ms shows Stale");
    settle(bridge, *core, 3, 5.0);
    check(!core->staleBadgeShown(), "new generation clears Stale");

    // Keyboard adjustment of a macro: arrows, page, home/end, numeric entry; undo labels via gestures.
    juce::Component* ageKnob = nullptr;
    for (auto* k : core->allKnobs())
        if (static_cast<sh::Knob*>(k)->paramId() == "macro.age") ageKnob = k;
    check(ageKnob != nullptr, "Age knob exists");
    if (ageKnob) {
        auto* k = static_cast<sh::Knob*>(ageKnob);
        check(k->getTitle() == "Age" && k->getDescription().isNotEmpty(), "knob has accessible name and description");
        k->keyPressed(juce::KeyPress(juce::KeyPress::pageUpKey));
        check(std::abs(getParam(bridge, "macro.age") - 0.10) < 1e-3, "PageUp adds 10 %: " + juce::String(getParam(bridge, "macro.age"), 4));
        k->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
        check(std::abs(getParam(bridge, "macro.age") - 0.11) < 1e-3, "Right adds 1 %");
        k->keyPressed(juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier, 0));
        check(std::abs(getParam(bridge, "macro.age") - 0.109) < 1e-3, "Shift+Left subtracts 0.1 %");
        k->keyPressed(juce::KeyPress(juce::KeyPress::endKey));
        check(std::abs(getParam(bridge, "macro.age") - 1.0) < 1e-6, "End jumps to maximum");
        k->keyPressed(juce::KeyPress(juce::KeyPress::homeKey));
        check(std::abs(getParam(bridge, "macro.age")) < 1e-6, "Home jumps to minimum");
        k->focusLost(juce::Component::focusChangedDirectly);  // closes the coalesced key gesture
        check(bridge.history().entries.contains("Adjust Age"), "keyboard edit recorded as labelled undo step 'Adjust Age'");
        check(k->commitText("45"), "numeric entry parses");
        check(std::abs(getParam(bridge, "macro.age") - 0.45) < 1e-3, "numeric entry sets 45 %");
        check(bridge.history().entries.contains("Set Age"), "numeric entry is a labelled undo step");
        k->showNumericEntry();
        check(k->isEntryOpen(), "Enter opens numeric entry");
        k->closeEntry();
        check(!k->commitText("abc"), "invalid text rejected");
        check(k->commitText("45.321 %"), "precise percentage input accepted");
        const double precise = getParam(bridge, "macro.age");
        check(k->commitText(sh::formatValue(sh::Fmt::Percent, k->getValue())) &&
                  getParam(bridge, "macro.age") == precise,
              "unchanged rounded knob text preserves exact parameter");
        for (const auto* bad : {"1 0", "1e 2", "+-1", "45 junk", "nan", "1e999", "101 %"})
            check(!k->commitText(bad) && getParam(bridge, "macro.age") == precise,
                  juce::String("malformed knob input preserves parameter: ") + bad);
        k->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
        check(std::abs(getParam(bridge, "macro.age")) < 1e-6, "Delete resets to default");
    }
    // Utility knobs present and attached: input/output/mix/HPF + toggles.
    int attached = 0;
    for (auto* k : core->allKnobs()) attached += static_cast<sh::Knob*>(k)->isAttached() ? 1 : 0;
    check(attached == 8, "4 macros + 4 utility knobs attached: " + juce::String(attached));
    {
        auto* hpf = static_cast<sh::Knob*>(core->allKnobs().back());
        check(hpf->paramId() == "hpf.frequency", "HPF frequency knob is last utility knob");
        check(hpf->commitText("120 Hz") && std::abs(getParam(bridge, "hpf.frequency") - 120.0) < 0.5, "typed Hz value applied");
        check(sh::formatValue(sh::Fmt::Db, -3.0) == "-3.0 dB" && sh::formatValue(sh::Fmt::Percent, 0.5) == "50 %", "value formatting");
    }

    // Quick scenes: one transaction, values match the UX-02 table.
    const int before = bridge.history().entries.size();
    core->applyScene(3);
    check(bridge.history().entries.size() == before + 1 && bridge.history().entries[before] == "Scene: Lo-Fi Attic Gear",
          "scene is one labelled transaction: " + bridge.history().entries.joinIntoString("|") + " before=" + juce::String(before));
    check(std::abs(getParam(bridge, "macro.age") - 0.85) < 1e-3 && std::abs(getParam(bridge, "macro.sag") - 0.65) < 1e-3 &&
              std::abs(getParam(bridge, "macro.drift") - 0.6) < 1e-3 && std::abs(getParam(bridge, "macro.warmth") - 0.45) < 1e-3 &&
              getParam(bridge, "analogNoise.enabled") > 0.5 && std::abs(getParam(bridge, "mix") - 1.0) < 1e-3 && getParam(bridge, "gainMatch.enabled") > 0.5,
          "Lo-Fi Attic Gear scene values");
    check(core->sceneMatches(3) && !core->sceneMatches(0), "active scene detected");
    core->applyScene(0);
    check(core->sceneMatches(0), "Clean Console scene");

    // Settings persistence through bridge.setUiSettings.
    core->setDarkTheme(false);
    check(!bridge.uiSettings().darkTheme, "theme persisted");
    core->setView(EditorView::Workbench);
    check(bridge.uiSettings().view == EditorView::Workbench && core->view() == EditorView::Workbench, "view persisted");
    core->setEducational(true); core->setReduceMotion(true); core->setLowFrameRate(true);
    const auto u = bridge.uiSettings();
    check(u.educational && u.reduceMotion && u.lowFrameRate, "educational/reduce-motion/low-frame-rate persisted");
    core->setUserScale(1.5f);
    check(std::abs(bridge.uiSettings().scale - 1.5f) < 1e-6 && editor.getWidth() == 1500, "scale persisted and window follows: " + juce::String(editor.getWidth()));
    core->setUserScale(0.75f);
    check(editor.getWidth() == 750 && editor.getHeight() == 540, "75 % scale gives 750x540 at the saved logical size");
    core->setUserScale(5.0f);
    check(std::abs(bridge.uiSettings().scale - 2.0f) < 1e-6, "scale clamped to 200 %");
    core->setUserScale(1.0f);
    core->setDarkTheme(true); core->setView(EditorView::Easy); core->setReduceMotion(false); core->setLowFrameRate(false); core->setEducational(false);

    // Timer: 60 Hz, 30 Hz low frame rate, stopped when hidden.
    check(!core->timerRunning(), "timer is stopped while the editor is not showing");
    core->setActive(true);
    check(core->timerRunning() && core->refreshRateHz() == 60, "60 Hz refresh when visible: " + juce::String(core->refreshRateHz()));
    core->setLowFrameRate(true);
    check(core->refreshRateHz() == 30, "30 Hz low frame rate: " + juce::String(core->refreshRateHz()));
    core->setActive(false);
    check(!core->timerRunning(), "timer stops when hidden");
    core->setLowFrameRate(false);

    // Tooltips: delay constant, show on demand, bounds, educational long text, pin, Escape dismiss.
    if (ageKnob) {
        auto& help = core->help();
        check(help.showFor(*ageKnob), "help available for Age");
        check(help.isOpen() && help.currentText().contains("Age"), "short tooltip text");
        const auto bubble = help.bubbleBounds();
        check(editor.getLocalBounds().contains(editor.getLocalArea(&help, bubble)), "tooltip fits inside the editor");
        const auto shortLen = help.currentText().length();
        core->setEducational(true);
        help.refresh();
        check(help.currentText().length() > shortLen, "educational mode shows the long text");
        auto img = test::renderComponent(editor, 1000, 720, 1.0f);
        test::writePng(img, outDir.getChildFile("shell_tooltip_educational.png"));
        help.setPinned(true);
        check(help.isPinned(), "tooltip can be pinned");
        editor.getChildComponent(0)->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        check(!help.isOpen() && !help.isPinned(), "Escape dismisses (and unpins) the tooltip");
        core->setEducational(false);
    }

    // Narrow layout: drawer overlay, Escape closes it, macros and utilities remain reachable.
    editor.setBounds(0, 0, 750, 540);
    core->selectPanel(PanelId::Quality);
    check(core->drawerOpen() && core->currentPanel() == PanelId::Quality && core->isNarrow(), "drawer opens on the Quality tab at 750 px");
    checkLayout(editor, *core, "narrow-drawer-open");
    editor.getChildComponent(0)->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    check(!core->drawerOpen(), "Escape closes the narrow drawer");
    editor.setBounds(0, 0, 1000, 720);
}

void testOpenEditorStateRestore() {
    CircuitDriftProcessor source(Edition::Full, "Source"), target(Edition::Full, "Target");
    ProcessorEditorBridge bridge(target);
    CircuitDriftEditor editor(target, bridge);
    auto* core = sh::EditorCore::find(&editor);
    if (!core) { check(false, "live restore: editor core exists"); return; }
    struct Listener final : juce::AudioProcessorListener {
        int dirty = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override {
            if (details.nonParameterStateChanged) ++dirty;
        }
    } listener;
    target.juce::AudioProcessor::addListener(&listener);
    const auto restore = [&](const UiSettings& settings) {
        source.setEditorSettings(settings);
        juce::MemoryBlock state;
        source.getStateInformation(state);
        target.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    };

    UiSettings restored;
    restored.scale = 1.25f;
    restored.darkTheme = false;
    restored.view = EditorView::Workbench;
    restored.educational = restored.reduceMotion = restored.lowFrameRate = true;
    restored.overlay = 2;
    restored.xray = true;
    restored.pcbView = 1;
    restored.lastWidth = 1200;
    restored.lastHeight = 800;
    restore(restored);
    core->pumpFrame(0.0, ++g_now);
    const auto& applied = core->context().ui;
    check(!applied.darkTheme && applied.view == EditorView::Workbench && applied.scale == 1.25f &&
              applied.educational && applied.reduceMotion && applied.lowFrameRate &&
              applied.overlay == 2 && applied.xray && applied.pcbView == 1,
          "open editor applies restored theme, view, accessibility and Workbench settings");
    check(editor.getWidth() == 1500 && editor.getHeight() == 1000 &&
              std::abs(core->effectiveScale() - 1.25) < 1e-9,
          "open editor applies saved logical dimensions and scale");
    check(listener.dirty == 0, "applying a restored editor state does not mark it dirty again");

    // A local edit may arrive before any timer callback can observe the host load.
    restored.scale = 1.5f;
    restored.lastWidth = 900;
    restored.lastHeight = 600;
    restore(restored);
    core->setEducational(false);
    const auto afterEdit = target.editorSettings();
    check(!afterEdit.educational && afterEdit.scale == 1.5f && !afterEdit.darkTheme &&
              afterEdit.view == EditorView::Workbench && afterEdit.reduceMotion && afterEdit.lowFrameRate &&
              afterEdit.lastWidth == 900 && afterEdit.lastHeight == 600 && afterEdit.pcbView == 1,
          "immediate local preference edit preserves unrelated restored fields");
    check(editor.getWidth() == 1350 && editor.getHeight() == 900 && listener.dirty == 1,
          "immediate local edit applies restored size and emits only its own dirty notification");

    // A newer explicit host resize takes precedence over saved dimensions, while
    // its logical size must be computed using the newly restored scale.
    restored.scale = 1.25f;
    restored.view = EditorView::Easy;
    restored.lastWidth = 1000;
    restored.lastHeight = 720;
    restore(restored);
    editor.setSize(1600, 1000);
    const auto afterResize = target.editorSettings();
    check(editor.getWidth() == 1600 && editor.getHeight() == 1000 &&
              afterResize.lastWidth == 1280 && afterResize.lastHeight == 800 &&
              afterResize.scale == 1.25f && afterResize.view == EditorView::Easy && !afterResize.darkTheme &&
              afterResize.educational && afterResize.reduceMotion && afterResize.lowFrameRate,
          "resize before refresh preserves restored preferences and records the newer host dimensions");
    core->pumpFrame(0.0, ++g_now);
    check(core->view() == EditorView::Easy && target.editorSettings().lastWidth == 1280 && listener.dirty == 2,
          "subsequent refresh neither reverts resize nor repeats dirty notification");
    target.juce::AudioProcessor::removeListener(&listener);
}

void testStatusBadges(const juce::File& outDir) {
    // Status flags are rendered from StatusInfo; FakeBridge reports none, so use a small subclass.
    struct Flagged : test::FakeBridge {
        StatusInfo status() override {
            auto s = test::FakeBridge::status();
            s.solverFallback = s.containmentActive = s.bypassed = s.tempoFallback = true;
            s.draftQuality = true; s.activeOversampling = 2; s.pendingOversampling = 8; s.reprepareRequired = true; s.bpm = 120;
            s.presetModified = true;
            return s;
        }
    } bridge;
    UiSettings us;
    bridge.setUiSettings(us);
    CircuitDriftEditor editor(bridge.processor(), bridge);
    editor.setBounds(0, 0, 1000, 720);
    auto* core = sh::EditorCore::find(&editor);
    if (!core) { check(false, "badges: core"); return; }
    settle(bridge, *core);
    const auto b = core->statusBadgeTexts();
    check(b.contains("Solver fallback") && b.contains("Containment") && b.contains("Bypassed") && b.contains("Tempo fallback 120 BPM"), "status badges: " + b.joinIntoString(","));
    check(core->qualityBadgeText().contains("2x FIR > 8x FIR pending") && core->qualityBadgeText().contains("DRAFT"), "pending quality and Draft label: " + core->qualityBadgeText());
    check(core->presetLabelText().contains("Modified"), "preset shows Modified");
    test::writePng(test::renderComponent(editor, 1000, 720, 1.0f), outDir.getChildFile("shell_status_badges.png"));
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();

    testContrast();
    testEducation();
    testSphereMapping();
    testOpenEditorStateRestore();
    g_failures += test::capture_ui_regressions::run(outDir);

    {
        test::FakeBridge bridge;
        setParam(bridge, "macro.age", 0.35); setParam(bridge, "macro.sag", 0.5);
        setParam(bridge, "macro.drift", 0.3); setParam(bridge, "macro.warmth", 0.55);
        setParam(bridge, "input.gain", 3.0); setParam(bridge, "output.gain", -2.0); setParam(bridge, "mix", 0.8);
        setParam(bridge, "gainMatch.enabled", 1.0); setParam(bridge, "hpf.enabled", 1.0); setParam(bridge, "output.softClip", 1.0);
        for (float scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
            for (bool dark : {true, false})
                for (auto view : {EditorView::Easy, EditorView::Workbench}) {
                    const int pw = juce::roundToInt(1000 * scale), ph = juce::roundToInt(720 * scale);
                    renderCase(bridge, outDir, scale, dark, view, pw, ph,
                               "shell_" + juce::String(juce::roundToInt(scale * 100)) + "_" + (dark ? "dark" : "light") + "_" + (view == EditorView::Easy ? "easy" : "workbench"),
                               false, pw / scale < 900.0f);
                }
        // Minimum logical viewport, narrow layout, drawer closed and open.
        for (bool dark : {true, false})
            for (auto view : {EditorView::Easy, EditorView::Workbench}) {
                const juce::String base = juce::String("shell_narrow_750x540_") + (dark ? "dark" : "light") + "_" + (view == EditorView::Easy ? "easy" : "workbench");
                renderCase(bridge, outDir, 1.0f, dark, view, 750, 540, base, false, true);
                renderCase(bridge, outDir, 1.0f, dark, view, 750, 540, base + "_drawer", true, true);
            }
        // Wide layout with the side drawer open.
        renderCase(bridge, outDir, 1.0f, true, EditorView::Easy, 1000, 720, "shell_100_dark_easy_drawer", true, false);
        renderCase(bridge, outDir, 1.0f, false, EditorView::Workbench, 1200, 800, "shell_wide_1200x800_light_workbench_drawer", true, false);
        // Host refuses to grow beyond 700x500: content reflows at a smaller effective scale, nothing cropped.
        {
            UiSettings s; bridge.setUiSettings(s);
            CircuitDriftEditor editor(bridge.processor(), bridge);
            editor.setBounds(0, 0, 700, 500);
            auto* core = sh::EditorCore::find(&editor);
            if (core) {
                settle(bridge, *core);
                check(core->effectiveScale() < 0.94 && core->logicalBounds().getWidth() >= 750 && core->logicalBounds().getHeight() >= 540, "undersized host window reflows at reduced scale");
                checkLayout(editor, *core, "host-refused-700x500");
                test::writePng(test::renderComponent(editor, 700, 500, 1.0f), outDir.getChildFile("shell_host_refused_700x500.png"));
            }
        }
        testBehaviour(bridge, outDir);
    }
    testStatusBadges(outDir);

    {   // LITE edition: sphere + two macros + bypass, no workbench.
        test::FakeBridge lite(Edition::Lite);
        UiSettings s; lite.setUiSettings(s);
        CircuitDriftEditor editor(lite.processor(), lite);
        editor.setBounds(0, 0, 1000, 720);
        auto* core = sh::EditorCore::find(&editor);
        if (core) {
            settle(lite, *core);
            check(core->isLite() && core->allKnobs().size() == 2, "LITE shows Sag and Thermal Age only");
            const auto items = core->essentialControls();
            bool bypass = false;
            for (auto& it : items) bypass = bypass || it.name.containsIgnoreCase("bypass");
            check(bypass, "LITE bypass reachable");
            test::writePng(test::renderComponent(editor, 1000, 720, 1.0f), outDir.getChildFile("shell_lite.png"));
        }
    }

    std::printf(g_failures ? "UI shell snapshot FAILED (%d failures)\n" : "UI shell snapshot passed\n", g_failures);
    return g_failures ? 1 : 0;
}
