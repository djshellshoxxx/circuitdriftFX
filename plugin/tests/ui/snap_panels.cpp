// Secondary panels: renders every panel with FakeBridge in both themes at 100 % and 200 % and at a
// narrow 360 px width, then checks layout invariants (no clipping, no overlap, text fits, every
// control has an accessible name and tooltip) and the required labels and interactions.
#include <cstdio>
#include "FakeBridge.h"
#include "PresetSelectionRegressions.h"
#include "ui/panels/PanelCommon.h"
#include "ui/panels/Panels.h"

using namespace cd::plugin;
namespace pc = cd::plugin::ui::pc;
using test::FakeBridge;

namespace {

int failures = 0;
juce::String currentCase;

void fail(const juce::String& msg) {
    ++failures;
    std::printf("FAIL [%s] %s\n", currentCase.toRawUTF8(), msg.toRawUTF8());
}
void expect(bool ok, const juce::String& msg) { if (!ok) fail(msg); }

bool isLeafControl(juce::Component* c) {
    return dynamic_cast<juce::Slider*>(c) || dynamic_cast<juce::ComboBox*>(c) || dynamic_cast<juce::Button*>(c) ||
           dynamic_cast<juce::TextEditor*>(c) || dynamic_cast<juce::ListBox*>(c) || dynamic_cast<juce::ProgressBar*>(c) ||
           dynamic_cast<juce::Label*>(c) || dynamic_cast<pc::NoteLabel*>(c);
}

// Collects all visible text, checks bounds/overlap/accessibility/text fit under `c`.
void inspect(juce::Component& c, juce::StringArray& texts, const juce::String& path) {
    const auto kids = c.getChildren();
    juce::Array<juce::Component*> vis;
    for (auto* k : kids) if (k->isVisible()) vis.add(k);
    for (int i = 0; i < vis.size(); ++i) {
        auto* k = vis[i];
        const auto where = path + "/" + (k->getTitle().isNotEmpty() ? k->getTitle() : juce::String(typeid(*k).name()));
        const auto b = k->getBounds();
        if (!c.getLocalBounds().contains(b)) fail("clipped outside parent: " + where);
        if (b.getWidth() <= 0 || b.getHeight() <= 0) fail("empty bounds: " + where);
        for (int j = i + 1; j < vis.size(); ++j)
            if (b.intersects(vis[j]->getBounds())) fail("overlap: " + where + " vs " + vis[j]->getTitle());
        if (auto* n = dynamic_cast<pc::NoteLabel*>(k)) { texts.add(n->text()); continue; }
        if (auto* l = dynamic_cast<juce::Label*>(k)) {
            texts.add(l->getText());
            if (pc::textWidth(l->getText(), 12.5f) > b.getWidth() + 1) fail("label text clipped: " + l->getText());
            continue;
        }
        if (auto* bt = dynamic_cast<juce::Button*>(k)) {
            texts.add(bt->getButtonText());
            const bool toggle = dynamic_cast<juce::ToggleButton*>(k) != nullptr;
            const int need = pc::textWidth(bt->getButtonText(), toggle ? 15.0f : 16.0f) + (toggle ? 30 : 16);
            if (need > b.getWidth()) fail("button text clipped: " + bt->getButtonText() + " need " + juce::String(need) + " have " + juce::String(b.getWidth()));
        }
        if (auto* cb = dynamic_cast<juce::ComboBox*>(k)) {
            for (int n = 0; n < cb->getNumItems(); ++n) {
                texts.add(cb->getItemText(n));
                if (pc::textWidth(cb->getItemText(n), 15.0f) + 34 > b.getWidth()) fail("combo item clipped: " + cb->getItemText(n));
            }
        }
        if (isLeafControl(k)) {
            if (k->getTitle().isEmpty()) fail("control without accessible name: " + where);
            if (auto* t = dynamic_cast<juce::SettableTooltipClient*>(k))
                if (t->getTooltip().isEmpty()) fail("control without tooltip: " + where);
            continue;
        }
        inspect(*k, texts, where);
    }
}

template <class T = juce::Component>
T* findByTitle(juce::Component& c, const juce::String& title) {
    for (auto* k : c.getChildren()) {
        if (k->getTitle() == title) if (auto* t = dynamic_cast<T*>(k)) return t;
        if (auto* r = findByTitle<T>(*k, title)) return r;
    }
    return nullptr;
}

bool has(const juce::StringArray& texts, const juce::String& needle) {
    for (const auto& t : texts) if (t.containsIgnoreCase(needle)) return true;
    return false;
}

StatusInfo richStatus(FakeBridge& b, bool modified) {
    auto s = b.status();
    s.presetModified = modified;
    s.tempoFallback = true;
    s.pitchHz = 220.0; s.pitchConfidence = 0.5;
    s.keyState = "Audio: low confidence";
    s.pendingOversampling = 8; s.reprepareRequired = true; s.draftQuality = false;
    s.activeOversampling = 2;
    s.harmonicAvailable[0] = false;
    s.pitchDriftActive = false;
    s.networkStatus = "Joined: 3 members";
    return s;
}

juce::Image renderTextEditor(juce::TextEditor& editor) {
    juce::Image image(juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true);
    juce::Graphics graphics(image);
    editor.paintEntireComponent(graphics, false);
    return image;
}

bool samePixels(const juce::Image& a, const juce::Image& b) {
    if (!a.isValid() || !b.isValid() || a.getBounds() != b.getBounds()) return false;
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return false;
    return true;
}

void expectGlyphColour(juce::TextEditor& editor, juce::Colour colour, const juce::String& message) {
    // findColour() was already correct in the broken implementation. Compare
    // the actual stored glyph rendering with the same field normalized to the
    // intended colour, without changing its text, history or caret/selection.
    const auto actual = renderTextEditor(editor);
    editor.applyColourToAllText(colour, false);
    expect(samePixels(actual, renderTextEditor(editor)), message);
}

class ThemeHistoryRegression final : private juce::Timer {
public:
    void run() {
        // An editor may outlive its FlowContent, or die before the content.
        // Dispatching the first queued edit below must not call a dead listener.
        survivor_.setBounds(0, 0, 240, 32);
        survivor_.onTextChange = [this] { ++survivorCallbacks_; };
        {
            pc::FlowContent owner;
            owner.addItem(survivor_, 240, 32);
            survivor_.insertTextAtCaret("Owner gone");
            auto early = std::make_unique<juce::TextEditor>();
            owner.addItem(*early, 240, 32);
            early.reset();
            owner.setTheme(ui::lightTheme());
        }

        // A queued notification follows the editor to its new parent. The old
        // content must neither retain a dangling registration nor recolour it.
        auto former = std::make_unique<pc::FlowContent>();
        former->setTheme(ui::lightTheme());
        moved_.setBounds(0, 0, 240, 32);
        moved_.setCaretVisible(false);
        former->addItem(moved_, 240, 32);
        moved_.insertTextAtCaret("Moved text");
        currentOwner_ = std::make_unique<pc::FlowContent>();
        currentOwner_->setTheme(ui::darkTheme());
        currentOwner_->addItem(moved_, 240, 32);
        former->setTheme(ui::lightTheme());
        former.reset();

        startTimer(5000);
        if (queueNext()) juce::MessageManager::getInstance()->runDispatchLoop();
        stopTimer();
    }

private:
    bool queueNext() {
        if (juce::MessageManager::callAsync([this] { advance(); })) return true;
        fail("could not queue theme-history regression");
        finish();
        return false;
    }
    void finish() {
        finished_ = true;
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
    void timerCallback() override { fail("theme-history dispatch timed out"); finish(); }
    void rememberEdit() {
        caret_ = editor_->getCaretPosition();
        selection_ = editor_->getHighlightedRegion();
    }
    void checkDelivered(const juce::String& text, const char* operation) {
        expect(callbacks_ > callbacksBefore_, juce::String(operation) + " delivers the existing text callback");
        expect(editor_->getText() == text, juce::String(operation) + " preserves the expected draft");
        expect(editor_->getCaretPosition() == caret_ && editor_->getHighlightedRegion() == selection_,
               juce::String(operation) + " listener preserves the edit's caret and selection");
        expectGlyphColour(*editor_, ui::themeFor(!initialDark_).text,
                          juce::String(operation) + " restores glyphs in the current theme");
    }
    void advance() {
        if (finished_) return;
        if (scenario_ == 4) { finish(); return; }
        initialDark_ = scenario_ >= 2;
        const bool deletion = (scenario_ % 2) != 0;
        currentCase = juce::String("theme-history-") + (initialDark_ ? "dark-to-light-" : "light-to-dark-") +
                      (deletion ? "deletion" : "insertion");
        if (phase_ == 0) {
            if (scenario_ == 0) {
                expect(survivorCallbacks_ == 1, "queued editor callback survives content destruction");
                expectGlyphColour(moved_, ui::darkTheme().text, "reparented editor uses its current content theme");
            }
            UiSettings settings; settings.darkTheme = initialDark_;
            bridge_.setUiSettings(settings);
            panel_ = ui::createPanel(ui::PanelId::Blueprint, bridge_);
            panel_->setBounds(0, 0, 420, 1400);
            editor_ = findByTitle<juce::TextEditor>(*panel_, "Blueprint JSON");
            if (editor_ == nullptr) { fail("theme-history blueprint editor missing"); finish(); return; }
            editor_->setCaretVisible(false);
            editor_->setFont(juce::Font(juce::FontOptions(22.0f)));
            editor_->setText(deletion ? "MMWW" : "MM", false);
            callbacks_ = 0;
            const auto original = editor_->onTextChange;
            editor_->onTextChange = [this, original] { ++callbacks_; if (original) original(); };
            if (deletion) editor_->setHighlightedRegion({2, 4});
            else editor_->setCaretPosition(2);
            editor_->insertTextAtCaret(deletion ? "" : "WW");
        } else if (phase_ == 1) {
            expect(callbacks_ > 0, "initial edit uses the real asynchronous callback");
            const auto draft = editor_->getText();
            editor_->setHighlightedRegion({0, 1});
            rememberEdit();
            UiSettings settings; settings.darkTheme = !initialDark_;
            panel_->settingsChanged(settings);
            expect(editor_->getText() == draft && editor_->getCaretPosition() == caret_ &&
                   editor_->getHighlightedRegion() == selection_, "theme switch preserves draft, caret and selection");
            callbacksBefore_ = callbacks_;
            expect(editor_->undo(), "theme switch preserves undo history");
            rememberEdit();
        } else if (phase_ == 2) {
            checkDelivered(deletion ? "MMWW" : "MM", "undo");
            callbacksBefore_ = callbacks_;
            expect(editor_->redo(), "colour normalization preserves redo history");
            rememberEdit();
        } else {
            checkDelivered(deletion ? "MM" : "MMWW", "redo");
            panel_.reset();
            editor_ = nullptr;
            ++scenario_;
            phase_ = -1;
        }
        ++phase_;
        queueNext();
    }

    FakeBridge bridge_;
    std::unique_ptr<ui::Panel> panel_;
    juce::TextEditor* editor_ = nullptr;
    juce::TextEditor survivor_, moved_;
    std::unique_ptr<pc::FlowContent> currentOwner_;
    juce::Range<int> selection_;
    int scenario_ = 0, phase_ = 0, caret_ = 0;
    int callbacks_ = 0, callbacksBefore_ = 0, survivorCallbacks_ = 0;
    bool initialDark_ = false, finished_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    FakeBridge bridge;

    // Every registry parameter the panels bind must exist in the processor tree.
    for (auto* id : {"quality.mode", "quality.oversampling", "quality.phase", "quality.zeroLatency", "output.softClip", "hpf.enabled", "ab.crossfade",
                     "slot.a.lifecycle", "slot.a.scenario", "key.source", "key.enabled", "key.root", "key.scale", "key.mask", "tempo.enabled",
                     "tempo.division", "tempo.modifier", "tempo.depth", "harmonics.target", "harmonics.amount", "transient.preserve",
                     "sidechain.source", "sidechain.depth", "sidechain.hpf", "pitchDrift.enabled", "network.enabled", "network.bleedEnabled",
                     "network.bleedDb"}) {
        currentCase = "params";
        expect(bridge.parameters().getParameter(id) != nullptr, juce::String("missing parameter ") + id);
    }

    const int widths[] = {420, 360};
    const float scales[] = {1.0f, 2.0f};
    for (int pi = 0; pi < static_cast<int>(ui::PanelId::Count); ++pi) {
        const auto id = static_cast<ui::PanelId>(pi);
        for (bool dark : {true, false}) {
            for (int w : widths) {
                for (float scale : scales) {
                    if (w == 360 && scale > 1.0f && !dark) continue;  // keep the matrix bounded
                    const juce::String name = juce::String(ui::panelTitle(id)).replace("/", "") + "_" + (dark ? "dark" : "light") + "_" +
                                              juce::String(juce::roundToInt(scale * 100)) + "pct_w" + juce::String(w);
                    currentCase = name;
                    auto panel = ui::createPanel(id, bridge);
                    if (!panel) { fail("createPanel returned null"); continue; }
                    UiSettings us; us.darkTheme = dark; us.scale = scale;
                    bridge.setUiSettings(us);
                    panel->settingsChanged(us);
                    const auto st = richStatus(bridge, true);
                    panel->setBounds(0, 0, w, 3000);
                    panel->refresh(st);
                    panel->refresh(st);
                    auto* base = dynamic_cast<pc::PanelBase*>(panel.get());
                    if (!base) { fail("not a PanelBase"); continue; }
                    const int ch = base->content().getHeight();
                    panel->setBounds(0, 0, w, std::max(200, ch));
                    panel->refresh(st);
                    expect(!base->viewport().getVerticalScrollBar().isVisible(), "scrollbar visible although content fits");
                    expect(base->content().getWidth() <= w, "content wider than panel");
                    juce::StringArray texts;
                    inspect(base->content(), texts, ui::panelTitle(id));
                    // Required labels per panel.
                    switch (id) {
                        case ui::PanelId::Quality:
                            expect(has(texts, "Off (1x) - Draft") && has(texts, "2x - Draft"), "Draft label on Off/2x");
                            expect(has(texts, "re-prepare") && has(texts, "Pending") && has(texts, "Active now") && has(texts, "Reset Warm") && has(texts, "Reset Cold"), "quality labels");
                            expect(has(texts, "samples") && has(texts, "Soft clip"), "latency/protection labels");
                            break;
                        case ui::PanelId::Community: expect(has(texts, "G06") && has(texts, "No network calls"), "G06 text"); break;
                        case ui::PanelId::Capture: expect(has(texts, "MP4 disabled") && has(texts, "G07"), "MP4 disabled reason"); break;
                        case ui::PanelId::Lifecycle:
                            expect(has(texts, "Commit Restoration") && has(texts, "Restored outcome") && has(texts, "Derelict outcome") && has(texts, "Display year") && has(texts, "live temperature"), "lifecycle labels");
                            break;
                        case ui::PanelId::Faults: expect(has(texts, "artistic bounded") && has(texts, "Clear all faults") && has(texts, "Apply fault"), "fault labels"); break;
                        case ui::PanelId::Blueprint: expect(has(texts, "State: Idle") && has(texts, "Validate"), "blueprint labels"); break;
                        case ui::PanelId::Musical:
                            expect(has(texts, "120 BPM assumed") && has(texts, "low confidence") && has(texts, "unavailable"), "musical status labels");
                            break;
                        case ui::PanelId::Network: expect(has(texts, "approximate") && has(texts, "Offline renders") && has(texts, "Joined"), "network limitation labels"); break;
                        case ui::PanelId::Presets: expect(has(texts, "[Modified]") && has(texts, "Load into Slot A"), "preset labels"); break;
                        case ui::PanelId::AB: expect(has(texts, "Audible") && has(texts, "Swap A and B"), "A/B labels"); break;
                        case ui::PanelId::History: expect(has(texts, "Undo") && has(texts, "Redo"), "history labels"); break;
                        default: break;
                    }
                    auto img = test::renderComponent(*panel, w, std::max(200, ch), scale);
                    if (!img.isValid() || !test::writePng(img, outDir.getChildFile("panels_" + name + ".png"))) fail("png write");
                }
            }
        }
    }

    // ---- Interactions -------------------------------------------------------------------------
    {
        currentCase = "interact-quality";
        auto p = ui::createPanel(ui::PanelId::Quality, bridge);
        p->setBounds(0, 0, 420, 1400);
        p->refresh(bridge.status());
        auto* os = findByTitle<juce::ComboBox>(*p, "Oversampling");
        expect(os != nullptr, "oversampling combo");
        if (os) {
            const int before = bridge.history().entries.size();
            os->setSelectedId(3, juce::sendNotificationSync);  // 2x
            auto* prm = bridge.parameters().getParameter("quality.oversampling");
            expect(prm && std::lround(prm->convertFrom0to1(prm->getValue())) == 2, "oversampling parameter set to 2x");
            expect(bridge.history().entries.size() == before + 1, "combo edit is one gesture");
            expect(os->getItemText(2).contains("Draft"), "2x is labelled Draft");
        }
    }
    {
        currentCase = "interact-lifecycle";
        auto p = ui::createPanel(ui::PanelId::Lifecycle, bridge);
        p->setBounds(0, 0, 420, 1400);
        p->refresh(bridge.status());
        auto* sl = findByTitle<juce::Slider>(*p, "Hardware Lifecycle");
        expect(sl != nullptr, "lifecycle slider");
        if (sl) {
            sl->setValue(0.5, juce::sendNotificationSync);
            auto* prm = bridge.parameters().getParameter("slot.a.lifecycle");
            expect(prm && std::abs(prm->getValue() - 0.5f) < 1e-3f, "lifecycle parameter follows scrubber");
            p->refresh(bridge.status());
            juce::StringArray texts;
            inspect(dynamic_cast<pc::PanelBase*>(p.get())->content(), texts, "lc");
            expect(has(texts, "Display year 2000"), "interpolated display year 2000 at 50 %");
        }
        auto* commit = findByTitle<juce::Button>(*p, "Commit Restoration");
        expect(commit != nullptr && !commit->isEnabled(), "Commit Restoration disabled for Derelict scenario");
    }
    {
        currentCase = "interact-blueprint";
        auto p = ui::createPanel(ui::PanelId::Blueprint, bridge);
        p->setBounds(0, 0, 420, 1400);
        p->refresh(bridge.status());
        auto* apply = findByTitle<juce::Button>(*p, "Apply...");
        auto* validate = findByTitle<juce::Button>(*p, "Validate");
        expect(apply && validate && !apply->isEnabled(), "apply disabled before validation");
        if (validate) if (validate->onClick) validate->onClick();
        p->refresh(bridge.status());
        expect(apply && apply->isEnabled(), "apply enabled once validation is Ready");
        juce::StringArray texts;
        inspect(dynamic_cast<pc::PanelBase*>(p.get())->content(), texts, "bp");
        expect(has(texts, "State: Ready") && has(texts, "Components 17"), "blueprint ready report");
    }
    {
        currentCase = "interact-faults";
        auto p = ui::createPanel(ui::PanelId::Faults, bridge);
        p->setBounds(0, 0, 420, 1600);
        p->refresh(bridge.status());
        auto* apply = findByTitle<juce::Button>(*p, "Apply fault");
        auto* type = findByTitle<juce::ComboBox>(*p, "Fault type");
        expect(type && type->getNumItems() == 2, "electrolytic offers exactly open and leaky");
        if (apply) if (apply->onClick) apply->onClick();
        p->refresh(bridge.status());
        expect(bridge.faults(0).size() == 1 && bridge.faults(0)[0].componentKey == "C_k", "fault applied to C_k");
        auto* comp = findByTitle<juce::ComboBox>(*p, "Component");
        if (comp) {
            comp->setSelectedId(static_cast<int>(engine::ComponentId::V1) + 1, juce::sendNotificationSync);
            expect(type && type->getNumItems() == 1 && type->getItemText(0).contains("Microphonic"), "V1 offers microphonic only");
            comp->setSelectedId(static_cast<int>(engine::ComponentId::T_out) + 1, juce::sendNotificationSync);
            expect(type && type->getNumItems() == 0 && apply && !apply->isEnabled(), "transformer has no fault model");
        }
    }
    {
        currentCase = "interact-musical";
        auto p = ui::createPanel(ui::PanelId::Musical, bridge);
        p->setBounds(0, 0, 420, 2400);
        p->refresh(bridge.status());
        auto* b = findByTitle<juce::Button>(*p, "Custom scale note C#");
        expect(b != nullptr, "custom mask button exists");
        auto* scale = findByTitle<juce::ComboBox>(*p, "Scale");
        if (scale && b) {
            scale->setSelectedId(6, juce::sendNotificationSync);
            p->refresh(bridge.status());
            expect(b->isEnabled(), "mask enabled for Custom scale");
            if (b->onClick) b->onClick();
            auto* prm = bridge.parameters().getParameter("key.mask");
            expect(prm && std::lround(prm->convertFrom0to1(prm->getValue())) == (4095 ^ 2), "mask bit cleared");
        }
    }
    {
        currentCase = "interact-presets";
        auto p = ui::createPanel(ui::PanelId::Presets, bridge);
        p->setBounds(0, 0, 420, 1400);
        p->refresh(bridge.status());
        auto* name = findByTitle<juce::TextEditor>(*p, "Preset name");
        auto* save = findByTitle<juce::Button>(*p, "Save");
        if (name && save) {
            name->setText("bad/name", false);
            if (save->onClick) save->onClick();
            juce::StringArray texts;
            inspect(dynamic_cast<pc::PanelBase*>(p.get())->content(), texts, "pr");
            expect(has(texts, "letters, digits"), "invalid preset name shows inline error");
        } else fail("preset name/save controls");
    }
    {
        currentCase = "interact-preset-search-selection";
        struct SelectionBridge final : FakeBridge {
            juce::String loadedId;
            bool loadPreset(const juce::String& id, int slot, juce::String& error) override {
                loadedId = id;
                return FakeBridge::loadPreset(id, slot, error);
            }
        } selectionBridge;
        juce::String error;
        expect(selectionBridge.savePreset("Other Scene", error), "create second user preset fixture");
        auto p = ui::createPanel(ui::PanelId::Presets, selectionBridge);
        p->setBounds(0, 0, 420, 1400);
        p->refresh(selectionBridge.status());
        auto* search = findByTitle<juce::TextEditor>(*p, "Search presets");
        auto* factory = findByTitle<juce::ListBox>(*p, "Factory presets");
        auto* user = findByTitle<juce::ListBox>(*p, "User presets");
        auto* load = findByTitle<juce::Button>(*p, "Load into Slot A");
        auto* remove = findByTitle<juce::Button>(*p, "Delete");
        auto* exportButton = findByTitle<juce::Button>(*p, "Export...");
        if (search && factory && user && load && remove && exportButton) {
            auto filter = [&](const juce::String& query) {
                search->setText(query, false);
                if (search->onTextChange) search->onTextChange();
            };
            auto expectCleared = [&] {
                expect(factory->getSelectedRow() < 0 && user->getSelectedRow() < 0,
                       "rebuilt results have no stale row selection");
                expect(!load->isEnabled() && !remove->isEnabled() && !exportButton->isEnabled(),
                       "actions stay disabled until a result is explicitly selected");
                selectionBridge.loadedId.clear();
                if (load->onClick) load->onClick();
                expect(selectionBridge.loadedId.isEmpty(), "load cannot use the previous result's id");
            };

            filter("My Scene");
            expect(user->getListBoxModel()->getNumRows() == 1, "first filter has one user result");
            user->selectRow(0);
            expect(load->isEnabled() && remove->isEnabled() && exportButton->isEnabled(),
                   "explicit user selection enables its actions");
            filter("Other Scene");
            expect(user->getListBoxModel()->getNumRows() == 1, "replacement filter also has one user result");
            expectCleared();
            user->selectRow(0);
            if (load->onClick) load->onClick();
            expect(selectionBridge.loadedId == "user:Other Scene", "reselection targets the new user result");

            filter("Clean");
            expect(factory->getListBoxModel()->getNumRows() == 1, "first filter has one factory result");
            factory->selectRow(0);
            expect(load->isEnabled() && exportButton->isEnabled() && !remove->isEnabled(),
                   "explicit factory selection remains read-only");
            filter("Mint");
            expect(factory->getListBoxModel()->getNumRows() == 1, "replacement filter also has one factory result");
            expectCleared();
            factory->selectRow(0);
            if (load->onClick) load->onClick();
            expect(selectionBridge.loadedId == "factory:mint-1968-console", "reselection targets the new factory result");
        } else fail("preset search/selection controls");
    }

    {
        currentCase = "preset-selection-identity";
        failures += test::preset_selection_regressions::run();
    }

    {
        currentCase = "theme-placeholder-colours";
        for (bool initialDark : {false, true}) {
            FakeBridge themedBridge;
            UiSettings settings; settings.darkTheme = initialDark;
            themedBridge.setUiSettings(settings);
            auto panel = ui::createPanel(ui::PanelId::Presets, themedBridge);
            panel->setBounds(0, 0, 420, 1400);
            settings.darkTheme = !initialDark;
            panel->settingsChanged(settings);
            for (const auto* title : {"Search presets", "Preset name"}) {
                auto* editor = findByTitle<juce::TextEditor>(*panel, title);
                expect(editor != nullptr && editor->isEmpty(), juce::String(title) + " starts empty");
                if (editor == nullptr) continue;
                editor->setCaretVisible(false);
                const auto actual = renderTextEditor(*editor);
                editor->setTextToShowWhenEmpty(editor->getTextToShowWhenEmpty(), ui::themeFor(!initialDark).textDim);
                expect(samePixels(actual, renderTextEditor(*editor)), juce::String(title) + " placeholder follows the current theme");
            }
        }
    }

    // Run exactly one message loop, last: JUCE_MODAL_LOOPS_PERMITTED is disabled.
    // Each assertion is queued after genuine editor notifications; no direct
    // onTextChange calls stand in for listener delivery in this regression.
    ThemeHistoryRegression themeHistory;
    themeHistory.run();

    std::printf(failures ? "UI panels snapshot FAILED (%d)\n" : "UI panels snapshot passed\n", failures);
    return failures ? 1 : 0;
}
