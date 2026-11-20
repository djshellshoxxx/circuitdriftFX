# Runtime and pitch-worker review — 2026-11-20 UTC

## Scope and source

This independent worker reviewed the runtime/control boundary from isolated editor
head `3f122dfa0f56706e26329eaa3fd31a3199145ce3`. The implementation is recorded in
`b66668aa38b01bb1db98eaa826b9b21c3fc0da44`.

Reviewed areas: the `edf0c21` pitch seek generation, MIDI chunk boundaries and
overflow release, zero-frame calls, audio-key activation, reset ownership, and
the relevant older detector/control-hub contracts. This was local source review
and bounded synthetic functional testing. No external targets or live hosts were
tested. Models remain synthetic; physical and detector-corpus gates remain open.

## Confirmed repairs

1. **Audio-key activity revived old pitch.** The accepted-input clock stops when
   Audio tracking is disabled or another source is selected. Consequently an old
   acquired result remained fresh when Audio tracking returned. Activity changes
   now request a producer-side generation change, including zero-frame changes.
   The worker owns the associated history reset. New tests require reacquisition
   from new audio after both disable/re-enable and source changes.
2. **Dropped input left pre-gap pitch usable.** A full input ring recorded a gap
   without immediately invalidating published pitch. The worker then reset its
   analysis but replayed the pre-gap queue, falsely reacquiring that old input.
   Dropped blocks now request the same epoch/boundary flush used for seeks. The
   boundary uses release/acquire ordering so a worker that observes a newer
   boundary also observes its corresponding committed write index.
3. **The runtime lacked a safe live reset entry point.** The existing `reset()`
   requires quiescent audio and pitch threads. New
   `Processor::resetRealtime(ResetKind)` resets audio-owned control/engine state
   between audio callbacks while the pitch worker can remain active. Prepared
   initial drift snapshots restore the seed sequence without preparation or
   allocation. Pitch buffers are never reset from the audio owner; their epoch
   is invalidated and the worker flushes itself. Existing quiescent semantics
   remain available. Warm/Cold engine semantics are unchanged.

The runtime also exposes an audio-owner `effectiveTargets(int)` getter for the
adapter's coherent UI snapshot publication. The adapter must call it from the
audio role rather than reading live engine state directly from the UI.

## Verification

GCC 14.2, C++20, CMake Release, ASan/UBSan/float-cast-overflow:

```sh
cmake -S . -B build -DCD_SANITIZE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target cd_control_pitch_tests cd_control_safety_tests cd_runtime_processor_tests -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build -R '^(control_pitch|control_safety|runtime_processor)$' --output-on-failure
```

All three suites passed in 19.58 seconds on the implementation commit, and again
in 13.64 seconds after the test scheduling refinements below. The runtime regressions cover both reset
kinds, sample-for-sample equivalence with quiescent reset, cleared MIDI state,
pitch invalidation while a worker runs, subsequent reacquisition and zero
intercepted C++ callback allocations. The pitch suite's worst pure-sine error
remained 12.05 cents against its existing 15-cent limit.

The new overflow test failed two assertions against the original detector
implementation; the repaired detector passed. The new activity test failed four
assertions with the original `ControlProcessor` implementation and passed with
the repair. These are ordinary finite tone blocks and mode changes.

The threaded transport test now reserves capacity for its entire finite input,
so delayed worker scheduling cannot accidentally turn it into a producer-drop
case. The separate small-ring test expressly verifies the drop/flush policy.
No accuracy or confidence threshold was changed.

LeakSanitizer is disabled because this environment runs under a tracer. These
results do not constitute leak qualification, a ThreadSanitizer result, native
plugin/host validation, or a measured realtime deadline. Full-engine reset work
is bounded by the prepared engine, but native worst-case reset latency is still
an acceptance item. Root must run the combined feature head after integration.

## Preserved follow-up work

- Public `PitchDetector::prepare` only checks a minimum rate. Its direct API
  still needs finite/maximum-rate validation and an explicit maximum ring
  capacity. `ControlProcessor` normalizes production sample rates, so this was
  recorded separately without widening this round's implementation scope.
- PR #10's detector has an ambiguity flag that suppresses High confidence for
  strong peaks unrelated to the chosen harmonic period. The newer detector
  has a different state/publication API and no equivalent explicit ambiguity
  flag. Corpus qualification and a contract-level confidence decision are still
  required; the old code is retained rather than copied over the new detector.
- PR #14's `ControlHub` includes generation/revision-stamped configuration,
  restore acknowledgements and stopped-audio checkpoint fallback. Its
  electrical/thermal/RNG capacities are explicitly placeholders, and it is not
  connected to this engine. Those semantics need a real persistence adapter;
  the presence of the newer processor does not prove that work redundant.
- The existing documented seek race may discard some newly accepted samples
  when a worker finishes an older chunk before observing the new epoch. Old
  results remain unusable. Reacquisition uses subsequent accepted audio.
- Full session checkpoints, pitch confidence corpus, native reset budgets and
  old branch reconciliation remain open. This review does not close those gates.

## Next owner

Integrate the implementation and this handoff without replacing competing work.
Publish UI reset requests atomically, consume them only from the audio owner,
and preserve the quiescent requirement for preparation/release and ordinary
`reset()`. The source branches remain intact. Review combined native plugin
results on the exact integrated SHA before making any release claim.
