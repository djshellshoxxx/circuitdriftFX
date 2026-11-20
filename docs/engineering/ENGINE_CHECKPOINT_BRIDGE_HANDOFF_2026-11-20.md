# Isolated G12 engine-only checkpoint bridge — 2026-11-20

## Source and ownership

Candidate based on draft PR #16 published head `338e952f2a69c401035adf0e0101e33ff181a2f5`, with coordinator head deliberately untouched while its exact-publication CI run [44781704691](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44781704691) was queued. The GIF R20 backend/UI/resource owners already have separate nonoverlapping source ownership; this candidate changes no GIF, editor, media, blueprint or preset files. PR #27 owns independent state-history replay fixes. Do not merge into the coordinator until the source and CI are qualified, active GIF owners have been rechecked, and the coordinator has a fresh live-ref guard.

## Material but deliberately partial G12 behavior

`runtime::Processor` now exposes an explicit **engine-only, quiescent** checkpoint staging bridge: `allocateEngineCheckpoint`, `captureEngineCheckpoint` and `restoreEngineCheckpoint`. Allocation is only available after successful prepare and is an off-audio operation. Capture and restore preserve the engine's existing preallocated, allocation-free, fail-closed layout validation and no-mutation-on-reject semantics. They fail when released or unprepared. Engine ownership remains in the runtime; no caller reaches into the engine object.

This is **not** full G12 session restore. These functions require host callbacks and the pitch worker to be stopped throughout and do not capture or restore control/key/tempo/pitch state, instance/variation seeds, runtime transport epochs, parameters, host state, blueprint identity or live graph state. Until a coordinated snapshot/ack/restore/migration transaction is built, plugin host save/load and sound-preset Load/Save must remain unavailable for dynamic sessions. The wrapper must never be invoked concurrently with `process()`, `prepare()`, `release()` or `runPitchWorker()`.

## Regression and qualification

`tests/runtime/test_processor.cpp` now asserts admission refusal before prepare/after release; capture of advanced engine sample state; rejection of mismatched layouts without changing state; and exact roundtrip of layout, sample index, dynamic doubles and RNG words on a prepared, stopped runtime. Existing realtime reset/pitch regression remains intact.

This patch received scoped source inspection, not executable Linux sanitized core, native plugin, Windows/pluginval, DAW, ThreadSanitizer, peak RSS or hardware-model qualification. Run the new exact-head CI and inspect the runtime_processor suite; do not inherit #16 qualification. The full control/seed/transport, stopped-audio host handoff and dynamic A/B transactions remain next ownership.

No original branch, main, release, deployment, paid service or unrelated service was modified.
