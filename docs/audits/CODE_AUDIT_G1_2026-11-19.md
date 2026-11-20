# Code audit generation 1 — 2026-11-19

## Scope and source

Source: PR #15, `0c0d6897caf36d4057ac3d8db8cfb346dd5412b4`.
Reviewed plugin adapters, host parameter mapping, UI gestures, asynchronous callback
lifetimes, and Windows UI failure evidence supplied by the coordinator. Runtime
integration uses the coordinator's `f413eb5` and MIDI timing correction `33100cf`.
Physical models remain synthetic fixtures; this audit does not close G01.

## Implemented

- Connected the previously pass-through JUCE processor to `cd_runtime`: cached
  parameter atomics, preallocated double audio/sidechain scratch, bounded blocks,
  sample-position/tempo/MIDI translation, reported latency and aligned host/LITE
  bypass. Float and double host callbacks use the same runtime path. A separate
  pitch-analysis thread is stopped and joined before runtime prepare/release.
- Limited MIDI inspection to 1024 events per host callback, including ignored
  SysEx. Overflow is observable and terminates the admitted prefix with
  all-notes-off so a dropped note-off cannot permanently latch key tracking.
  This effect consumes MIDI; it does not forward incoming messages as output.
- Rejected malformed bus-count layouts before indexing bus arrays. Rejected null,
  negative-length, and larger-than-4-MiB plugin state inputs before parsing XML.
- Pinned a Workbench gesture to the parameters selected at its beginning. Changing
  A/B selection no longer redirects a live gesture into the other slot; rollback
  and host begin/end notifications use the same captured parameters. Empty,
  duplicate, unlisted and non-finite edits are handled explicitly.
- Closed host/undo gestures when a panel binding is replaced or destroyed. A
  continuing old drag cannot write to the replacement binding, and complete edits
  within a live gesture do not nest notifications.
- Guarded the asynchronous settings-menu callback with its component weak pointer
  before accessing the editor implementation. This is a source-confirmed lifetime
  repair; no GUI event-order reproduction is claimed yet.
- Replaced locale-dependent, NUL-terminated numeric parsing with a length-bounded
  decimal parser. A pre-fix executable reproduced `1 0` being accepted as 10,
  `1e308 M` as infinity, and an embedded-NUL suffix being ignored. Those inputs
  now fail without editing a parameter; ordinary engineering-unit input remains.
- Shortened Faults component-selector captions by removing parenthesized model
  qualifiers, while retaining full names in selected-item tooltip/description and
  notes. This addresses the Windows `T_out - Output transformer (magnetizing
  branch)` clipping failure at the supported 360 px width without weakening the
  existing layout assertion.

## Verification at this checkpoint

- `plugin/tests/test_ui_input.cpp`: direct GCC C++20 build with ASan/UBSan passed
  using `ASAN_OPTIONS=detect_leaks=0`. LeakSanitizer itself cannot run under this
  execution environment's ptrace; no leak-validation claim is made. The test is
  also registered as `ui_numeric_input` in the ordinary core CTest gate.
- `plugin/tests/test_plugin_params.cpp`: added real-JUCE host gesture regression
  coverage, malformed buses/state lengths, oversized float/double blocks, declared
  latency and output trim, delayed host bypass, unchanged sidechain input, MIDI
  consumption and bounded overflow. These tests replace the obsolete
  pass-through expectation. Their JUCE build/run is **pending** at this checkpoint.
- Existing `ui_snap_panels` still checks all component captions at every tested
  width/theme/scale; no assertion or required matrix case was removed.
- Exact pinned JUCE, CLAP extension and CLAP submodules were obtained and verified.
  Task-local Linux native dependencies were prepared. Single-job GCC configuration
  reached JUCE's `juceaide` build, then compilation of `juce_gui_basics_3.cpp` was
  killed under memory pressure. A single retry using lower-memory GCC garbage
  collection settings compiled that file and the remaining GUI sources, then
  was killed compiling bundled Harfbuzz. A separate bounded syntax-only command
  over the changed JUCE sources was also killed. Local plugin attempts stopped
  there; no successful plugin compilation, Windows run or release qualification
  is claimed. Use the exact-commit Windows CI result for the next qualification.

## Remaining work for a fresh worker

1. Independently review the combined runtime/host adapter, especially sample-clock
   overflow/seek behavior, MIDI offset and overflow semantics, worker lifecycle,
   zero-frame calls, and allocation/lock freedom in actual JUCE callbacks.
2. Run Windows plugin consistency, pluginval and all UI snapshots at the exact new
   commit; fix any regression rather than accepting the old pass-through test.
3. Implement a real `EditorBridge` for the custom editor and supported services.
   `createEditor()` still uses JUCE's Generic editor. The custom UI is currently
   exercised through `FakeBridge` only; it must not be advertised as live product
   functionality. Do not connect a fake/no-op service bridge merely to show it.
4. Complete dynamic runtime checkpoint/session persistence, a validated tail
   reporting policy, and user-visible staged-quality/reprepare diagnostics.
   APVTS saves parameter configuration only. A whole-product completion or release
   claim would be incorrect.
5. Qualify real host IPC, calibrated hardware models and remaining release gates
   separately. No deployment, release publication or acceptance threshold change
   occurred in this audit.
