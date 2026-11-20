# Live editor review — 2026-11-20

Reviewed the isolated live editor at `3f122dfa0f56706e26329eaa3fd31a3199145ce3`, concentrating on ordinary host parameter edits, undo,
editor lifetime and truthful service availability. The processor snapshot/reset
integration is owned by the coordinator and is required by this repair commit.

## Confirmed defects and repairs

1. **Undo included unrelated automation during an open gesture.** The live bridge
   previously compared snapshots of every exposed parameter at begin/end. An
   automated B-slot value changing while the user edited A therefore entered the
   same undo step. `EditorBridge::touchParameter` now declares the exact parameters
   before their first write. Every production panel, shell, Workbench and scene
   path declares its writes, and copy/swap also use explicit targets. Repeated
   declarations retain the first value. Regressions exercise real `ParamBinding`
   and `ParamOps` against APVTS while an unrelated parameter changes.
2. **Keyboard, wheel, numeric and reset knob edits lacked matching host gesture
   boundaries.** `SliderParameterAttachment` only supplies those boundaries for
   actual slider drag notifications. The shell's direct `setValue` paths now
   bracket complete edits or coalesce keyboard/wheel edits into one host gesture.
   Beginning a mouse edit first closes the previous keyboard gesture.
3. **Destroying a dragging knob left the host gesture open and retained callbacks
   into destroyed derived members.** The knob now closes the outstanding host and
   bridge gestures while both are alive, removes callbacks, then detaches JUCE.
   Tests destroy an actively dragged knob without mouse-up and count both host
   and undo begin/end notifications.
4. **Live quality/control status and effective component targets were incomplete.**
   The bridge now reads the coordinator's immutable `EditorSnapshot` and maps
   active/pending quality, staged prepare requirements, bypass, reset/fallback,
   tempo, key, pitch, sidechain, harmonic availability and effective targets.
   Historical clamp counters are deliberately not described as current emergency
   containment activity.
5. **Warm/Cold buttons advertised an unavailable no-op.** The bridge now requests
   the coordinator's bounded runtime reset transaction. Feature availability is
   false while the Full processor is unprepared or for LITE. The quality panel
   refreshes availability and disables both actions when unavailable. The new
   adapter/runtime path must be qualified with the coordinator's tests.

## Validation at the review commit

`git diff --check` passed. Exact pinned JUCE source was inspected at
`dabd5ad13cd869cba41046947fffc8e5ce5cd662`. A GCC C++20 syntax pass over the
changed UI sources and plugin consistency tests was attempted, but `cc1plus`
was killed by the shared environment before it completed. **No completed native
compile or executed JUCE test is claimed by this worker checkpoint.** The
coordinator must record the combined linked build and execution results after
integrating the matching processor snapshot/reset interface. Required targets
are `plugin_param_consistency` and `ui_snap_live_bridge`, plus affected shell,
panel and Workbench suites.

## Boundaries and next review

- APVTS publishes individual parameter values. A/B copy/swap is one local undo
  record but not an atomic multi-parameter audio-block transaction. Concurrent
  host writes to the same parameter are not serialized by this UI adapter.
- Stale undo replay checks every touched current value before writing. That
  preflight cannot exclude a subsequent host write from another thread.
- User preferences, presets, blueprint application, faults, restoration seeds,
  native capture and full session continuity remain separate work. The original
  service failures and restoration availability checks remain intact.
- Current containment activity needs a dedicated published engine field; lifetime
  totals do not establish that a limiter/fallback is active now.
- Native host/window behavior, real hardware calibration and release performance
  are not inferred from this source review or the synthetic UI regression source.

## Follow-up: processor snapshot/reset review and preference ownership

The UI preference bridge now delegates to `CircuitDriftProcessor::editorSettings`
and `setEditorSettings`, retaining the existing finite/range bounds before
publication. The coordinator owns the matching non-audio synchronization and
host-state serialization. These changes require that interface when integrated.

Read-only review of the coordinator's in-progress `PluginProcessor` snapshot and
reset patch found no new memory-lifetime defect in the reviewed implementation:

- The persistent `TripleBuffer<EditorSnapshot>` belongs to the processor and is
  never reconstructed during runtime preparation. The single UI reader therefore
  copies stable storage while release/reprepare changes runtime ownership.
- Generation matching prevents a saved request for an older prepared runtime
  from being applied to a newly prepared runtime. Repeated reset clicks coalesce;
  a reset is applied only when the host next calls the audio processor.
- The callback copies telemetry/control/targets into the producer slot and
  publishes atomically. UI code does not dereference the runtime's mutable state.
- Reset requests add no lock, allocation, file I/O or worker-thread wait in the
  adapter's audio path. The runtime implementation and its worker ownership must
  still be tested independently.

This is source review of the snapshot/reset patch, not a concurrency execution
result. It assumes the normal host contract that `prepareToPlay` and
`releaseResources` do not run concurrently with `processBlock`, and the declared
single message-thread snapshot consumer. A read overlapping release may return
a snapshot taken just before release; it cannot expose freed runtime storage.
The preferences/host-state serialization was not yet present when this follow-up
was reviewed, so that part requires the coordinator's final verification.
