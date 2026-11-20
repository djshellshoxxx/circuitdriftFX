# Control continuation snapshots

`control::ControlSnapshot` captures the control processor's dynamic state as a
fixed-size typed C++ value. It is one prerequisite for G12 session continuity and
future A/B operations. It is not a host session, a sound preset, an engine
checkpoint, or a serialized format. The caller owns the parameter snapshot and
future audio, MIDI, tempo and transport inputs.

## Supported continuation

`ControlProcessor::captureSnapshot`, `canRestoreSnapshot` and `restoreSnapshot`
return an explicit `ControlSnapshotResult`. The first implementation supports
Manual and MIDI selection with no pending, in-use or retained harmonic map.
Audio selection is rejected even when key tracking is disabled. A pitch worker
may keep running because these methods neither read nor change its mutable state
or generation. Audio-worker and harmonic-map continuity need separate contracts.

Compatibility requires the same snapshot version, effective prepared sample rate,
maximum block size and instance seed. The seed also identifies the prepared
reset origin. Versions cover both state shape and continuation algorithms; a
future incompatible change must increase `ControlSnapshot::kVersion`.

An accepted restore reproduces subsequent control targets, valid per-sample
output spans and status for identical future parameters, inputs and callback
partitioning while operating in the supported domain. It does not promise that
different callback partitions produce identical block-end targets. Rejected
restores leave all live state unchanged, and failed captures leave the supplied
destination unchanged. No fallback reset is performed implicitly.

## Ownership and bounded work

Call these methods from the single audio owner between processing calls, or from
a non-audio owner while audio is quiescent. Preparation, resets, tuning/budget
setters and harmonic-map publication must not overlap a snapshot operation. The
candidate supplied for admission/restore must remain unchanged throughout it.

Capture fills a local fixed-size value and validates it before assigning the
destination. Restore admits the complete aggregate and every component before
writing any member. Component admission depends only on the fixed candidate and
unchanged receiver preparation, so the following bounded commit has no late
failure path. There are no allocation, locking, file or worker-wait operations.
Drift clock validation uses a fixed 64-step calculation, rather than replaying
the historical sample count. The note table has a fixed capacity of 32.

## Captured state

| Component | Continuation state |
|---|---|
| Each creative drift | Effective sample rate, RNG words and Gaussian spare, three stochastic processes, cached output, slewed delay and sample/tick clocks |
| Each resonance quantizer | Scale/root, separate note-decision/output initialization, current decision, glide destination/start/current pitch and remaining time |
| Each slot | Key blend, delay hold, cached cutoff/delay targets and last quantized offset |
| Tempo breathing | Phase and seek correction, smoothed recovery, enabled/PPQ history and complete cached output |
| Key tracking | Ordered bounded note table, sustain pedal, reference pitch/weight, state and hold/return timers |
| Transient detector | Fast/slow envelopes, held strength and hold counter |
| Sidechain load | Effective rate, HPF setting and both channel histories, mean square, envelope, applied budget and cached ratio |
| Processor | Pending effective budget, tuning, control status and source selection |

Pending published settings remain distinct from previously applied state. For
example, a budget setter can run after the last block; capture preserves both
the future setting and the sidechain detector's historical applied budget.
The pending budget is normalized exactly as `SidechainLoad::setBudgetAmps`:
nonfinite/nonpositive values become zero and positive values are capped at
`DBL_MAX/8`. This preserves its future effect without changing live state during
capture. A pending tuning change likewise does not rewrite cached reference
outputs.

The two drift clocks are independent because the public `drift()` accessor
permits separate advances. Engine sample indices are not part of this value and
are not assumed to equal either clock. Reset restarts from the receiver's
matching prepared origin.

Scratch audio buffers are excluded; the next positive block overwrites its full
valid output span. Restore invalidates previously returned output views. There
are no meaningful output samples in a zero-frame call. Harmonic-map pointers,
pitch-worker history and external lifetimes are never copied into the value.

## Component API limits

The six component snapshot APIs can also be used independently. Creative drift
requires matching effective sample rates but permits same-rate transfer across
different seeds and slots. The aggregate intentionally has a stricter seed
identity to preserve its reset behavior. Key snapshots admit pitches within
±32768, covering the processor's finite tuning domain; this does not promise
round trips for arbitrary larger values passed directly to `advanceAudio`.

These C++ values must not be loaded by copying arbitrary serialized bytes into
them. Padding, ABI, enum/bool representations and schema migration require an
explicit future serializer. Existing engine and host state formats are unchanged
by this control-only API, and G12 remains open until coordinated runtime capture,
explicit incompatible-session fallback and host persistence are integrated.

Execution evidence and independent review outcomes are recorded separately with
the source commits to which they apply; this API description is not a test result.
