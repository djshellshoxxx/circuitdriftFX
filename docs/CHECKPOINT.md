# Current checkpoint — 2026-11-19

The active consolidation candidate is `codex/cdfx-consolidation-20261119`, based on
PR #15 with selected unique work from PR #10. Read the current
[consolidation handoff](engineering/CONSOLIDATION_HANDOFF.md),
[source inventory](engineering/consolidation-state.json), and audit records in
`docs/audits/` before choosing or reporting work.

The current source has an integrated circuit/control runtime, JUCE adapter,
configuration/history/queue primitives, media/network/community modules and recovered
blueprint/lifecycle/fault primitives. The custom editor still uses a fake service
bridge in tests; the live processor uses its generic parameter editor. Complete
session continuity, real service/provider integration, physical calibration and
native host qualification remain open. The historical claims below describe the
older integration only and must not be used as current source or test status.

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
