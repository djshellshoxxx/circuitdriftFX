# Integration review: reset, telemetry, and editor-state persistence

## Scope and evidence

Fresh source review of the continuation tree at `7d55df6`, including the
coordinator's then-uncommitted `PluginProcessor` and `EditorState.h` changes.
The review covered the persistent editor snapshot, generation-stamped reset
request, runtime/control reset, editor bridge, host-state preference encoding,
and the actual shell/Workbench consumers. A separate fresh reviewer examined
the preference lifecycle; the integration reviewer checked those findings
against the source before reporting them.

The coordinator subsequently authorized focused repairs to the shell and
Workbench. Those source changes and regression cases are described below.
Review workers did not run builds or native tests, because the coordinator
owned the combined build and its memory budget. `git diff --check` passed.
This document does not claim a native-host or physical-model qualification.

## Findings and source repairs

### IR-01 — An open editor did not apply restored preferences

**Priority: P2.** `PluginProcessor::setStateInformation` installed decoded
settings, but `shell::EditorCore` read `bridge.uiSettings()` only at
construction. Its next `persist()` could write cached shell settings over a
newly loaded host state.

Repair: `EditorCore` now compares the bridge preferences with its last persisted
snapshot. It synchronizes on frame refresh, activation, local preference
changes, and resize events. Applying restored settings updates theme, view,
scale/layout, accessibility, and Workbench settings without writing them back.
A reentrancy guard suppresses writes from the resize caused by restoration.
An explicit newer host resize keeps that physical size and computes its logical
dimensions using the restored scale. The settings-menu callback synchronizes
before evaluating a toggle, including when a host load occurs while the menu
is open.

`snap_shell` now uses real processors and the production bridge to cover:

- Restoring a session into an already open editor.
- Applying saved scale, dimensions, theme, view, accessibility, and Workbench
  preferences.
- A local preference edit before the next timer tick, preserving unrelated
  restored fields.
- A resize before the next timer tick, preserving restored preferences while
  retaining the newer explicit dimensions.
- No dirty-state notification from applying restored settings; one notification
  per actual local edit/resize, with none added by the following refresh.

Native execution of those regression cases remains the coordinator's gate.

### IR-02 — Workbench projection was serialized but disconnected

**Priority: P2.** The new XML format serialized `uiPcbView`, but the Workbench
buttons only changed the canvas directly. `PcbCanvas::applySettings` ignored
the field, so it neither saved nor restored the selected projection.

Repair: view buttons persist the selection through the bridge, and the canvas
applies a saved mode only when it differs from the current mode. Theme and
accessibility changes, and reselection of the current view, retain the existing
orbit/pan/zoom. Existing snapshot cases select their projection through settings.

`snap_workbench` now checks all three views through real host-state
serialization/restoration, the selected toolbar button, and preservation of a
deliberately changed orbit. Native execution remains pending at this review
checkpoint.

### IR-03 — Preference changes lacked host dirty-state notification

**Priority: P2, host-dependent.** `setEditorSettings` initially changed the
stored struct without notifying the host that persistent nonparameter state had
changed. Explicit saves still retrieved the settings, but some hosts could miss
the need to save the project.

The coordinator repaired this using the pinned JUCE
`withNonParameterStateChanged(true)` change flag, after releasing the settings
mutex, suppressing identical assignments. Host-state restoration writes the
validated settings separately and does not issue this user-edit notification.
The shell regression additionally checks that applying those restored settings
does not produce a notification indirectly through layout persistence.

### IR-04 — Zero-frame callbacks combined new controls with old diagnostics

**Priority: P2.** The engine's zero-frame path called `beginBlock` without
publishing telemetry. The adapter subsequently copied cached engine telemetry
together with new control status and targets. A reset or staged quality edit
could leave `needsReset` or `reprepareRequired` stale.

The coordinator owns the zero-frame publication repair and its engine/adapter
regressions. At this source-review checkpoint, the engine has been changed to
publish after the zero-frame update. Validation must verify that diagnostics
change while the sample index and other audio-driven clocks do not advance.

## Source review observations without new findings

- Persistent triple-buffer storage removes UI access to runtime-owned memory
  during preparation/release. The single UI-reader contract still applies.
- Generation-stamped reset requests prevent a stale request from applying to a
  different prepared runtime. Repeated requests intentionally coalesce.
- Audio-owned control reset is separated from pitch-worker analysis reset. The
  worker observes a discontinuity and resets its own analysis state.
- Optional root XML preference attributes preserve legacy parameter-only state
  and the existing `PARAM` child validation. Numeric, enum, and boolean checks
  occur before parameter replacement.
- Full dynamic session snapshots, coherent A/B engine-state transfer,
  restoration seeds, native media/provider services, and physical calibration
  remain outside this milestone.

## Remaining validation boundary

The combined native build must exercise `plugin_param_consistency`,
`ui_snap_shell`, `ui_snap_workbench`, and the other affected UI suites on the
final integrated source. Source review alone does not establish runtime success.
The coordinator's final validation record owns the actual results.

Useful lifecycle cases include a queued Cold reset followed by
release/reprepare, stale snapshot rejection before a new generation's first
publication, and UI reads overlapping serialized host preparation/release.
No new memory-lifetime defect was established in the reset mailbox or persistent
snapshot design under their documented ownership assumptions.
