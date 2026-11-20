// Presets (CD-F24), A/B (CD-F24) and History (CD-F09) panels.
#include "ui/panels/PanelCommon.h"
#include "ui/workbench/GestureMath.h"

namespace cd::plugin::ui::pc {
namespace {

// ------------------------------------------------------------------------------------ Presets
struct PresetList : juce::ListBoxModel {
    std::vector<PresetEntry> rows;
    std::function<void(const PresetEntry*)> onSelect;
    std::function<void(const PresetEntry&)> onOpen;
    juce::ListBox* box = nullptr;
    int getNumRows() override { return static_cast<int>(rows.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        if (row < 0 || row >= getNumRows() || box == nullptr) return;
        const Theme& t = themeOf(*box);
        if (selected) g.fillAll(t.accent);
        const auto fg = selected ? t.textInverse : t.text;
        const auto& p = rows[static_cast<std::size_t>(row)];
        g.setColour(fg);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawText(p.name, 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged(int row) override {
        if (onSelect) onSelect(row >= 0 && row < getNumRows() ? &rows[static_cast<std::size_t>(row)] : nullptr);
    }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        if (onOpen && row >= 0 && row < getNumRows()) onOpen(rows[static_cast<std::size_t>(row)]);
    }
    juce::String getNameForRow(int row) override {
        return row >= 0 && row < getNumRows() ? rows[static_cast<std::size_t>(row)].name : juce::String();
    }
};

bool validPresetName(const juce::String& n, juce::String& err) {
    const auto name = n;
    if (name.isEmpty()) { err = "Enter a preset name."; return false; }
    if (name.toRawUTF8() == nullptr || name.getNumBytesAsUTF8() > 80) { err = "Name is longer than 80 characters."; return false; }
    for (auto c : name) {
        const bool ok = (c < 128) && (juce::CharacterFunctions::isLetterOrDigit(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')');
        if (!ok) { err = "Use letters, digits, space, - _ . ( ) only."; return false; }
    }
    if (name.startsWithChar(' ') || name.endsWithChar(' ') || name.startsWithChar('.') || name.endsWithChar('.') || name.contains("..")) {
        err = "No leading/trailing space or dot, and no \"..\"."; return false;
    }
    static const char* const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                           "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const auto stem = name.upToFirstOccurrenceOf(".", false, false).toUpperCase();
    for (auto* r : reserved) if (stem == r) { err = "That name is reserved by Windows."; return false; }
    return true;
}

class PresetsPanel final : public PanelBase {
public:
    explicit PresetsPanel(EditorBridge& b) : PanelBase(b) {
        setTitle("Presets");
        heading("Presets");
        current_ = note("", Tone::Normal);
        tip(search_, "Search presets", "Filter factory and user presets by name");
        search_.setTextToShowWhenEmpty("Search presets", content_.theme().textDim);
        search_.onTextChange = [this] { rebuild(); };
        content_.addItem(search_, 240, 28, true);
        refresh_ = makeButton("Refresh", "Rescan the local preset folder for external changes");
        refresh_->onClick = [this] { bridge.refreshPresets(); rebuild(); };
        content_.addItem(*refresh_, buttonBasis("Refresh"), 30);
        catalogNotice_ = note("", Tone::Warn);
        catalogNotice_->setTitle("Preset catalog status");

        for (auto* l : {&factory_, &user_}) {
            l->model.box = &l->box;
            l->box.setModel(&l->model);
            l->box.setRowHeight(24);
            l->box.setOutlineThickness(1);
            l->box.setMultipleSelectionEnabled(false);
        }
        tip(factory_.box, "Factory presets", "Read-only factory presets; select one, then Load");
        tip(user_.box, "User presets", "Your saved presets; select one, then Load, Export or Delete");
        factory_.title = heading("Factory");
        content_.addItem(factory_.box, 240, 116, true);
        user_.title = heading("User");
        content_.addItem(user_.box, 240, 116, true);
        factory_.model.onSelect = [this](const PresetEntry* p) { if (p) { user_.box.deselectAllRows(); setSelected(*p); } else if (user_.box.getSelectedRow() < 0) clearSelection(); };
        user_.model.onSelect = [this](const PresetEntry* p) { if (p) { factory_.box.deselectAllRows(); setSelected(*p); } else if (factory_.box.getSelectedRow() < 0) clearSelection(); };
        factory_.model.onOpen = user_.model.onOpen = [this](const PresetEntry&) { doLoad(); };

        load_ = makeButton("Load into Slot A", "Load the selected preset into the slot being edited");
        remove_ = makeButton("Delete", "Delete the selected user preset (asks first)");
        import_ = makeButton("Import...", "Import a preset file into your user presets");
        export_ = makeButton("Export...", "Export the selected preset to a file");
        load_->onClick = [this] { doLoad(); };
        remove_->onClick = [this] { doDelete(); };
        import_->onClick = [this] { doImport(); };
        export_->onClick = [this] { doExport(); };
        content_.addItem(*load_, buttonBasis("Load into Slot A"), 30);
        content_.addItem(*remove_, buttonBasis("Delete"), 30);
        content_.addItem(*import_, buttonBasis("Import..."), 30);
        content_.addItem(*export_, buttonBasis("Export..."), 30);

        heading("Save current sound");
        tip(name_, "Preset name", "Name for the new user preset");
        name_.setTextToShowWhenEmpty("Preset name", content_.theme().textDim);
        name_.setInputRestrictions(80);
        content_.addItem(name_, 200, 28);
        save_ = makeButton("Save", "Save the current settings as a user preset with this name");
        save_->onClick = [this] { doSave(); };
        content_.addItem(*save_, buttonBasis("Save"), 30);
        error_ = note("", Tone::Danger);
        info_ = note("Presets are local files. Loading replaces the sound of the edit slot and is one undo step.", Tone::Dim);
        updateButtons();
        rebuild();
    }

    void refresh(const StatusInfo& s) override {
        bridge.pollPresets();
        const auto catalog = bridge.presetCatalogStatus();
        if (catalog.generation != catalogGeneration_) rebuild();
        else updateCatalogNotice(catalog);
        bool changed = false;
        const juce::String slot = s.editSlot == 0 ? "A" : "B";
        if (slot != slot_) {
            slot_ = slot;
            load_->setButtonText("Load into Slot " + slot);
            load_->setTitle("Load into Slot " + slot);
        }
        changed |= current_->set("Current: " + (s.presetName.isEmpty() ? juce::String("(unsaved)") : s.presetName) +
                                     (s.presetModified ? "   [Modified]" : "   [Unmodified]"),
                                 s.presetModified ? Tone::Warn : Tone::Normal);
        if (changed) relayout();
    }

private:
    struct Pane { juce::ListBox box; PresetList model; NoteLabel* title = nullptr; };

    void rebuild() {
        const auto all = bridge.listPresets(search_.getText());
        // ListBox retains selections by row index when its content changes.
        // A filter or new async snapshot may put a different preset at that
        // index, so require an explicit selection before acting on new rows.
        factory_.box.deselectAllRows();
        user_.box.deselectAllRows();
        clearSelection();
        factory_.model.rows.clear();
        user_.model.rows.clear();
        for (const auto& p : all) (p.factory ? factory_ : user_).model.rows.push_back(p);
        factory_.box.updateContent();
        user_.box.updateContent();
        updateButtons();
        const auto catalog = bridge.presetCatalogStatus();
        catalogGeneration_ = catalog.generation;
        updateCatalogNotice(catalog);
        repaint();
    }
    void updateCatalogNotice(const PresetCatalogStatus& catalog) {
        juce::String message;
        if (catalog.loading)
            message = catalog.generation == 0 ? "Loading local presets..." : "Refreshing presets; showing the previous catalog...";
        if (catalog.error.isNotEmpty()) {
            if (message.isNotEmpty()) message += "\n";
            message += catalog.error;
        }
        if (catalog.truncated) {
            if (message.isNotEmpty()) message += "\n";
            message += "Catalog limit reached. Some files were not scanned; reduce this folder and refresh.";
        }
        const auto tone = catalog.error.isNotEmpty() ? Tone::Danger : catalog.truncated ? Tone::Warn : Tone::Dim;
        if (catalogNotice_->set(message, tone)) relayout();
    }
    void setSelected(const PresetEntry& p) {
        selected_ = p;
        hasSel_ = true;
        error_->set("");
        updateButtons();
        relayout();
    }
    void clearSelection() { hasSel_ = false; updateButtons(); }
    void updateButtons() {
        load_->setEnabled(hasSel_);
        export_->setEnabled(hasSel_);
        remove_->setEnabled(hasSel_ && !selected_.factory);
        remove_->setTooltip(hasSel_ && selected_.factory ? "Factory presets cannot be deleted" : "Delete the selected user preset (asks first)");
    }
    void fail(const juce::String& e) { error_->set(e); relayout(); }
    void doLoad() {
        if (!hasSel_) return;
        juce::String err;
        if (!bridge.loadPreset(selected_.id, bridge.status().editSlot, err)) fail("Could not load: " + err); else { error_->set(""); relayout(); }
    }
    void doSave() {
        juce::String err;
        const auto n = name_.getText();
        if (!validPresetName(n, err)) { fail(err); return; }
        if (!bridge.savePreset(n, err)) { fail("Could not save: " + err); return; }
        name_.clear();
        error_->set("");
        rebuild();
        relayout();
    }
    void doDelete() {
        if (!hasSel_ || selected_.factory) return;
        const auto id = selected_.id;
        auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::WarningIcon, "Delete preset",
                                                                 "Delete user preset \"" + selected_.name + "\"? This cannot be undone.",
                                                                 "Delete", "Cancel", this);
        juce::Component::SafePointer<PresetsPanel> safe(this);
        juce::AlertWindow::showAsync(opts, [safe, id](int r) {
            if (safe == nullptr || r != 1) return;
            juce::String err;
            if (!safe->bridge.deletePreset(id, err)) safe->fail("Could not delete: " + err);
            else { safe->clearSelection(); safe->error_->set(""); safe->rebuild(); safe->relayout(); }
        });
    }
    void doImport() {
        chooser_ = std::make_unique<juce::FileChooser>("Import preset", juce::File(), "*.json;*.circuitdrift.json");
        juce::Component::SafePointer<PresetsPanel> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe](const juce::FileChooser& fc) {
            if (safe == nullptr) return;
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            juce::String err;
            if (!safe->bridge.importPreset(f, err)) safe->fail("Import failed: " + err);
            else { safe->error_->set(""); safe->rebuild(); safe->relayout(); }
        });
    }
    void doExport() {
        if (!hasSel_) return;
        const auto id = selected_.id;
        chooser_ = std::make_unique<juce::FileChooser>("Export preset", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                                             .getChildFile(selected_.name + ".circuitdrift.json"),
                                                       "*.json;*.circuitdrift.json");
        juce::Component::SafePointer<PresetsPanel> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, id](const juce::FileChooser& fc) {
                                  if (safe == nullptr) return;
                                  const auto f = fc.getResult();
                                  if (f == juce::File()) return;
                                  juce::String err;
                                  if (!safe->bridge.exportPreset(id, f, err)) safe->fail("Export failed: " + err);
                                  else { safe->error_->set(""); safe->relayout(); }
                              });
    }

    juce::TextEditor search_, name_;
    Pane factory_, user_;
    std::unique_ptr<juce::TextButton> load_, remove_, import_, export_, save_, refresh_;
    NoteLabel *current_ = nullptr, *error_ = nullptr, *info_ = nullptr, *catalogNotice_ = nullptr;
    PresetEntry selected_;
    bool hasSel_ = false;
    std::uint64_t catalogGeneration_ = 0;
    juce::String slot_ = "A";
    std::unique_ptr<juce::FileChooser> chooser_;
};

// ---------------------------------------------------------------------------------------- A/B
class ABPanel final : public PanelBase {
public:
    explicit ABPanel(EditorBridge& b) : PanelBase(b), cross_(b, "A/B crossfade", "Blend between slot A (left) and slot B (right) in the audio path"),
                                        editA_("Edit A"), editB_("Edit B") {
        setTitle("A/B");
        heading("A/B comparison");
        note("Two complete sound slots. The edit slot decides which one the controls change; the crossfade decides what you hear.");
        heading("Edit slot");
        for (auto* t : {&editA_, &editB_}) {
            t->setClickingTogglesState(true);
            t->setRadioGroupId(7001);
            t->setWantsKeyboardFocus(true);
        }
        tip(editA_, "Edit slot A", "Make the detailed controls edit slot A");
        tip(editB_, "Edit slot B", "Make the detailed controls edit slot B");
        editA_.onClick = [this] { if (editA_.getToggleState()) bridge.setEditSlot(0); };
        editB_.onClick = [this] { if (editB_.getToggleState()) bridge.setEditSlot(1); };
        editA_.setToggleState(true, juce::dontSendNotification);
        content_.addItem(editA_, 100, 32);
        content_.addItem(editB_, 100, 32);
        heading("Copy and swap");
        copyAB_ = makeButton("Copy A to B", "Overwrite slot B with the sound of slot A");
        copyBA_ = makeButton("Copy B to A", "Overwrite slot A with the sound of slot B");
        swap_ = makeButton("Swap A and B", "Exchange the sounds of slots A and B");
        copyAB_->onClick = [this] { bridge.copySlot(0, 1); };
        copyBA_->onClick = [this] { bridge.copySlot(1, 0); };
        swap_->onClick = [this] { bridge.swapSlots(); };
        content_.addItem(*copyAB_, buttonBasis("Copy A to B"), 30);
        content_.addItem(*copyBA_, buttonBasis("Copy B to A"), 30);
        content_.addItem(*swap_, buttonBasis("Swap A and B"), 30);
        heading("Audible mix");
        cross_.bind("ab.crossfade");
        cross_.setDisplay([](double v) {
            const int b = juce::roundToInt(100.0 * v);
            return juce::String(100 - b) + "% A / " + juce::String(b) + "% B";
        }, gesture::parseCrossfade);
        cross_.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 110, 22);
        content_.addItem(cross_, 240, ParamSlider::kHeight, true);
        audible_ = note("", Tone::Normal);
        note("Copy, swap and crossfade changes are undoable. Both slots share one latency so switching never glitches the host.");
    }
    void refresh(const StatusInfo& s) override {
        editA_.setToggleState(s.editSlot == 0, juce::dontSendNotification);
        editB_.setToggleState(s.editSlot == 1, juce::dontSendNotification);
        const double x = cross_.binding.bound() ? cross_.binding.value() : 0.0;
        juce::String t = x <= 0.001 ? "Audible: slot A only" : x >= 0.999 ? "Audible: slot B only" : "Audible: blend of A and B (" + juce::String(juce::roundToInt(100.0 * (1.0 - x))) + "% A)";
        const int audible = x <= 0.001 ? 0 : x >= 0.999 ? 1 : -1;
        t += "   |   Editing: slot " + juce::String(s.editSlot == 0 ? "A" : "B") + (audible >= 0 && audible != s.editSlot ? " (not the audible slot)" : "");
        if (audible_->set(t, Tone::Normal)) relayout();
    }

private:
    ParamSlider cross_;
    juce::TextButton editA_, editB_;
    std::unique_ptr<juce::TextButton> copyAB_, copyBA_, swap_;
    NoteLabel* audible_ = nullptr;
};

// ------------------------------------------------------------------------------------ History
class HistoryPanel final : public PanelBase, private juce::ListBoxModel {
public:
    explicit HistoryPanel(EditorBridge& b) : PanelBase(b) {
        setTitle("History");
        heading("Undo history");
        undo_ = makeButton("Undo", "Undo the last completed change");
        redo_ = makeButton("Redo", "Redo the change that was undone");
        undo_->onClick = [this] { bridge.undo(); poll(true); };
        redo_->onClick = [this] { bridge.redo(); poll(true); };
        content_.addItem(*undo_, 200, 30);
        content_.addItem(*redo_, 200, 30);
        list_.setModel(this);
        list_.setRowHeight(24);
        list_.setOutlineThickness(1);
        list_.setRowSelectedOnMouseDown(false);
        tip(list_, "History entries", "Completed changes, oldest first; the highlighted row is the current state");
        content_.addItem(list_, 240, 220, true);
        note("One drag, one preset load or one copy is one entry. Entries below the highlighted row were undone and can be redone.");
        poll(true);
    }
    void refresh(const StatusInfo&) override { poll(false); }

private:
    void poll(bool force) {
        auto h = bridge.history();
        const bool same = !force && h.entries == h_.entries && h.position == h_.position && h.canUndo == h_.canUndo &&
                          h.canRedo == h_.canRedo && h.undoLabel == h_.undoLabel && h.redoLabel == h_.redoLabel;
        if (same) return;
        h_ = h;
        auto capt = [](const juce::String& base, const juce::String& l) { return l.isEmpty() ? base : base + ": " + l; };
        const auto ut = capt("Undo", h.undoLabel), rt = capt("Redo", h.redoLabel);
        undo_->setButtonText(ut); undo_->setTitle(ut);
        redo_->setButtonText(rt); redo_->setTitle(rt);
        undo_->setEnabled(h.canUndo);
        redo_->setEnabled(h.canRedo);
        list_.updateContent();
        list_.selectRow(h.position, false, true);
        list_.scrollToEnsureRowIsOnscreen(h.position);
        relayout();
    }
    int getNumRows() override { return h_.entries.size() + 1; }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int hgt, bool) override {
        const Theme& t = content_.theme();
        const bool cur = row == h_.position;
        if (cur) g.fillAll(t.accent);
        g.setColour(cur ? t.textInverse : (row <= h_.position ? t.text : t.textDim));
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        juce::String s = row == 0 ? juce::String("Initial state") : juce::String(row) + ". " + h_.entries[row - 1];
        if (cur) s += "   (current)";
        else if (row > h_.position) s += "   (undone)";
        g.drawText(s, 8, 0, w - 16, hgt, juce::Justification::centredLeft, true);
    }
    juce::String getNameForRow(int row) override { return row == 0 ? juce::String("Initial state") : h_.entries[juce::jlimit(0, h_.entries.size() - 1, row - 1)]; }

    std::unique_ptr<juce::TextButton> undo_, redo_;
    juce::ListBox list_;
    HistoryInfo h_;
};

}  // namespace

std::unique_ptr<Panel> makePresetsPanel(EditorBridge& b) { return std::make_unique<PresetsPanel>(b); }
std::unique_ptr<Panel> makeABPanel(EditorBridge& b) { return std::make_unique<ABPanel>(b); }
std::unique_ptr<Panel> makeHistoryPanel(EditorBridge& b) { return std::make_unique<HistoryPanel>(b); }

}  // namespace cd::plugin::ui::pc
