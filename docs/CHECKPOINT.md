# Current checkpoint — 2026-11-20

The active consolidation candidate is draft [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16),
branch `codex/cdfx-consolidation-20261119`. Read the current
[consolidation handoff](engineering/CONSOLIDATION_HANDOFF.md),
[source inventory](engineering/consolidation-state.json), and audit records in
`docs/audits/` before choosing or reporting work. Original branches and main are
preserved; included child PR #17–#25 are closed. Older PR #9/#10/#14 still need
semantic reconciliation.

Current source includes the integrated circuit/control runtime, live custom JUCE
editor, configuration/history/queue primitives, background preset discovery and
file operations, Blueprint validation preview, host-persisted UI preferences and
silent GIF capture. Coherent host-state restoration now includes a bounded
fallback handoff for stopped-audio notification failures. Exact source 1a3f8f3
passes 6/6 native suites in 59.50 s; all three fresh R28 reviews completed without
new actionable findings in scope. See the [host-state evidence](evidence/host-restore-2026-11-20-r26.json).

R30 repairs preserve ignored-MIDI processing, key glides/reachable cutoff notes
and sidechain history/numeric bounds. Source 00e2957 passed 11/11 sanitized suites
in 129.24 s. The revised engine algorithm identity rejects legacy checkpoints.
New fixed-size control snapshots support Manual/MIDI with no harmonic maps;
Audio selection is explicitly excluded. The independent component baseline
exposed missing drift sample-rate identity, which is repaired. Source 3fb0846
passes the component suite and nine affected suites: 10/10 in 21.15 s. See the
[control audit](audits/CONTROL_CHECKPOINTS_2026-11-20_R29_R33.md) and
[API contract](engineering/CONTROL_SNAPSHOTS.md).

All R29–R32 workers completed. Fresh R33 code, aggregate bug/test and defensive
workers are active; the aggregate compiles, and its independent behavioral suite
is pending. The current six native plugin/editor suites pass in 57.40 s at the
same source 3fb0846. Root owns builds and integration.
The published PR checkpoint remains 6f26b388 until this work qualifies and the
guarded update is verified. Full dynamic session/seed continuity, coordinated sound
presets and dynamic A/B, graph Apply, remaining providers/MP4, physical calibration,
Windows validator/pluginval and real-host/performance qualification remain open.
Historical claims below retain older provenance and are not current test status.

## Historical checkpoint retained for provenance

# Checkpoint

Read [HANDOFF.md](HANDOFF.md) for ownership and restart packets, [INTEGRATION_REVIEW.md](INTEGRATION_REVIEW.md) for source SHAs, reviewed defects and verification limits, and [TRACEABILITY.md](TRACEABILITY.md) for requirement status.

- Integration branch: `codex/reconcile-core-safety-20261030`; coordinator Codex. Source inputs: main `c10addecb48849d64890864f65f6ae231869be53`, PR #4 `a9c633fd89585a61780f206e303fced1c79326d1`, handoff PR #2 `c3b73d95b117b979be0c761573c49046a3df6e9e`. Original branches are preserved.
- Integrated: existing cd_core/cd_dsp registry/thermal/protection; main fixed 77-tap halfband and acceptance test; PR #4 configurable FIR/IIR cascade, synthetic manifests/MNA and cd_state documents. The configurable FIR class is named PolyphaseHalfband2x to coexist with main's Halfband2x.
- Corrected: JSON locale/identity/duplicate-lookup handling, parameter NUL aliases, model/feature recognition, publication boundaries, local temporary-file safety, numerical preparation/finite-state checks, and linked-channel gain API. Original sample-based gain matching remains supported.
- Executed locally: GCC 14.2 C++20 ASan/UBSan/float-cast-overflow direct build of 9 production sources and 5 executables; all 5 suites passed; registry freshness and docs validation passed. CMake unavailable locally; LeakSanitizer cannot run under this sandbox's ptrace configuration, so local leak detection was disabled. GitHub CMake/CTest for the integration commit must be read from its checks, not inferred from these local runs.
- T21 remains Partial: hot synthetic 4x FIR alias −53.13 dB versus proposed −70 dB target. Nominal 4x −150.33 dB and 16x −150.19 dB; these are synthetic tanh fixtures, not calibrated hardware.
- State remains Partial: supported configuration model version 0.0.1-dev; no audio transaction queue, snapshot publication/capture wiring, full A/B/history, or full blueprint validator. Scene publication rejects unsupported opaque payloads until semantic validators exist; no cloud upload occurs.
- NOT qualified: Windows/macOS execution (including Windows file replacement), host wrappers/GUI, one-hour realtime traps, physical accuracy, measured CPU/memory targets or release readiness.
- Next: establish provider contracts and canonical writer for P04 queue/snapshot and UX-09; integrate existing MNA/oversampler with emergency containment and latency handling. Do not recreate current modules.
- Gates remain G01 physical references/calibration, G03/G04 resource/alias budgets, G06–G09 access/licensing/dependency choices, and G12 full checkpoint/model migration. No paid provider, credential/security changes, release or deployment was made.
