# Code review generation 2 — 2026-11-19

## Scope and source

Independent continuation from `ba8eb98bc3266e38f9616409d59fdbabb7de2196`,
following `CODE_AUDIT_G1_2026-11-19.md`. Reviewed the framework-independent
runtime, JUCE processor adapter, and changed UI gesture/numeric paths against
JUCE `dabd5ad13cd869cba41046947fffc8e5ce5cd662` (the pinned source). Circuit
models remain synthetic fixtures; G01 and product/host qualification stay open.

## Confirmed runtime corrections

- **Preserve the active latency mode in the control producer.** The engine already
  stages zero-latency and pitch-delay changes until preparation, but the control
  producer previously obeyed the requested values immediately. Requesting zero
  latency while pitch delay was active changed the rendered signal despite the
  active configuration remaining unchanged. The runtime now keeps the prepared
  flags for control generation and forwards requested flags to the engine for
  pending-mode telemetry. A paired seeded render reproduced the difference before
  this fix and is identical afterward; reprepare then installs the requested mode.
- **Terminate a truncated MIDI prefix.** The JUCE adapter already did this, but
  direct runtime callers could lose a note-off after event 1024 and leave key
  tracking latched. The runtime reserves one bounded event for all-notes-off on
  overflow. Its timestamp is inside the supplied block and remains ordered after
  the admitted prefix. A 1025-event regression reproduced the active-note latch
  before the fix and verifies release afterward.
- **Do not subdivide LITE audio for ignored MIDI.** LITE now skips MIDI before
  choosing control boundaries. The seeded full-equivalence test supplies an
  ignored LITE note and checks identical audio and identical publication counts.
  The previous unnecessary boundary was source-confirmed; an audible LITE
  difference is not claimed for the current fixed topology.

## Framework and lifecycle review

- Pinned `juce_AudioProcessor.h::getBusBuffer` constructs non-owning views.
  `juce_AudioSampleBuffer.h::allocateChannels` uses 32 inline channel-pointer
  entries, so the supported mono/stereo views do not allocate. The adapter copies
  input and sidechain into prepared double scratch before writing the main output;
  float and double callbacks use the same path.
- Pinned `juce_Thread.cpp::stopThread(-1)` waits until the worker has exited.
  Adapter prepare/release/destruction joins the worker before changing runtime
  preparation or storage. No audio callback starts/stops the worker.
- MIDI iteration uses the JUCE buffer metadata view, avoids constructing owning
  messages, limits inspected records including SysEx, and clears output MIDI.
  The runtime's MIDI-boundary arithmetic stays within the nonnegative `int`
  frame count, and its signed/unsigned sample-offset additions saturate.
- State parsing and APVTS copying run outside the audio callback. Dynamic engine
  state is not saved by APVTS; no exact-session persistence claim is made.

## Numeric-entry corrections

- Panel and shell parsers still removed arbitrary characters before conversion:
  `1 0` became `10`, and malformed suffixes were silently discarded. The same
  path affected lifecycle/fault percentages, fault frequency/Q and capture
  duration. These controls now use the existing length-bounded decimal/unit
  parser, validate their ranges, and preserve the current value on rejection.
- The displayed A/B text `50% A / 50% B` previously became `5050` (clamped to B),
  while `Odd 50 %` became positive full-scale and `Even 50 %` parsed as zero.
  Explicit inverses now preserve A/B and harmonic meaning. A/B labels also use
  complementary rounded percentages so they always total 100.
- Submitting an unchanged rounded label preserves the exact underlying value.
  This prevents a value such as 0.373 from being quantized merely by pressing
  Enter on its display. The shared parser also rejects the previously accepted
  double sign `+-1`.
- These are source-confirmed JUCE write paths: pinned
  `juce_Slider.cpp::Pimpl::textChanged()` feeds the parser result into `setValue`
  with synchronous notification. No local executed-GUI reproduction is claimed.

## Verification at this checkpoint

- Configured and built `cd_runtime_processor_tests` and `cd_ui_input_tests` with
  GCC 14, C++20, ASan and UBSan. The initial two suites passed (7.30 seconds).
- Added regressions before runtime changes: the excess-MIDI release and staged
  latency render assertions both failed on the original source. The corrected
  runtime suite passes. Allocation interception remains active over oversized,
  null-input and zero-frame processing and reports zero callback allocations.
- Exact pinned-JUCE **header/source syntax compilation** of `PluginProcessor.cpp`,
  `ParameterLayout.cpp`, and `test_plugin_params.cpp` passed with GCC C++20.
  This is not a linked plugin, executed GUI, host run, or Windows build.
- Sanitizer runs use `ASAN_OPTIONS=detect_leaks=0`; this environment cannot run
  LeakSanitizer under its tracing restrictions. No leak-check result is claimed.
- Full JUCE builds are not retried here: the prior single-job build was killed
  under memory pressure. The coordinator is responsible for exact-commit Windows
  CI and plugin/UI qualification.
- Extended pure numeric tests pass with ASan/UBSan, including malformed text,
  non-finite/overflowing numbers, engineering units, A/B percentage validation,
  and signed harmonic targets. All seven changed JUCE UI implementation files,
  the plugin consistency test with production-panel regressions, and the shell
  snapshot test pass syntax compilation against the pinned JUCE headers. The
  production-widget regressions still require the linked Windows test run.

## Remaining work for the next checkpoint/worker

1. Run the new production-widget numeric regressions in linked JUCE tests and
   retain all existing UI layout/snapshot assertions.
2. Exercise zero-frame/mono/stereo/auxiliary buffer combinations, repeated worker
   reprepare/release, and configuration restore in linked JUCE tests on Windows.
3. Persist/validate edition and mapping identity without guessing the provenance
   of ambiguous legacy state. APVTS is currently parameter configuration only.
4. The seek epoch reaches the engine, but the runtime does not clear the control
   pitch/tempo history. A safe worker-aware epoch handoff is still required;
   calling `ControlProcessor::reset()` from audio would race the pitch worker.
5. Connect a real custom-editor/service bridge, staged-quality diagnostics,
   validated tail reporting, and complete session checkpoints. Generic JUCE
   editor and synthetic test bridges do not satisfy these product requirements.
