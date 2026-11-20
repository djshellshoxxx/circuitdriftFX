# Preset selection and catalog feedback review — 2026-11-20, R14

Base: `c6ebb22daab9c46edffa761b21f3fd45320e5074`.

## Confirmed selection defect

`PresetsPanel::rebuild()` previously replaced each list's rows without reconciling
the selected row index and the cached `PresetEntry`. JUCE's pinned
`ListBox::updateContent()` removes selected indexes that fall outside the new row
count; it does not report a selection change when an existing index now names a
different item. Filtering, importing, or reordering a catalog could therefore
leave one preset highlighted while load, export, and delete used an older ID.
Clicking the already selected index did not necessarily repair the mismatch.

The double-click handler also discarded its explicit row argument and called
`doLoad()` with the previously cached selection.

## Change

- Before replacing rows, save the selected stable ID and clear both list
  selections and the cached entry.
- After rebuilding, restore the selection only when that ID is present in the
  visible catalog. Refresh its cached metadata from the new row. Otherwise leave
  both lists unselected and their selection-dependent actions disabled.
- A double-click now selects its explicit row by ID before loading it, including
  when the other list owned the prior selection.
- Show `EditorBridge::presetCatalogMessage()` in a separate warning note after
  every rebuild. A complete scan clears the note; selecting or loading a preset
  does not hide a directory error or an incomplete-scan diagnostic. The bridge
  interface and real catalog report are supplied by the coordinated R14 changes.

These changes run on the UI message thread and do not change sound application,
undo semantics, host state, parameter IDs, or the audio path.

## Regression coverage

`plugin/tests/ui/PresetSelectionRegressions.h` exercises the production preset
panel with an explicitly synthetic tracking bridge. It is included in the existing
`cd_ui_snap_panels` executable without weakening its layout or interaction checks.

The regression checks:

1. No implicit selection when the panel opens.
2. Filtering a selected ID out while its old row index remains valid clears both
   lists and load/export/delete availability; a stale load callback dispatches
   nothing.
3. A retained ID follows its new index after filtering and reordering and remains
   the target passed to `loadPreset`.
4. Removing the selected entry clears a still-valid numeric row and cached action
   target.
5. User and factory selections preserve the factory delete restriction.
6. Double-clicking either list loads its explicit row exactly once and transfers
   the visual selection from the other list.
7. Empty results clear every selection-dependent action.
8. Incomplete scans and directory errors remain visible, and a subsequent complete
   scan clears the catalog diagnostic.

The tracking bridge records only the requested action ID. It does not implement
or validate actual preset sound loading, file choosers, deletion, or undo. Live
preset sound load/save remain separate work pending their seed/blueprint
transaction support.

## Validation

- `git diff --check`: passed.
- Source review checked the selection-notification behavior against the pinned
  JUCE `juce_ListBox.cpp` implementation.
- The coordinator will compile and execute the integrated `cd_ui_snap_panels`
  target with the new bridge interface. No full native build was run in this
  isolated worktree.
