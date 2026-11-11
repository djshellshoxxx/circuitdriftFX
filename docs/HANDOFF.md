# Handoff: reviewed core integration

Read [CLAUDE.md](../CLAUDE.md), [CHECKPOINT.md](CHECKPOINT.md), [INTEGRATION_REVIEW.md](INTEGRATION_REVIEW.md), [TRACEABILITY.md](TRACEABILITY.md), then the [implementation plan](../tasks/FEATURE_IMPLEMENTATION_PLAN.md).

## Source and ownership

This integration combines main `c10addecb48849d64890864f65f6ae231869be53`, PR #4 `a9c633fd89585a61780f206e303fced1c79326d1`, and the refreshed handoff from PR #2 `c3b73d95b117b979be0c761573c49046a3df6e9e`. These are the revisions inspected; recheck live refs before further work. PR #1 and PR #3 are already merged. Claude's original branches remain preserved.

Codex is integration owner for the user-approved reconciliation and safety fixes. During this change the writer boundaries were core JSON/state, oversampling, solver, and coordinator-owned CMake/tests/docs. There is no blanket claim of ownership over every unimplemented module. Agree a writer handoff before starting independent solver, state or instrumentation work on another branch.

## What exists

- `cd_core`: stable 74-parameter registry and bounded JSON parsing/writing.
- `cd_dsp`: thermal network (with step status), protection primitives, fixed and configurable halfband/oversampling, synthetic manifests, MNA solver.
- `cd_control`: macros, transient/sidechain detectors, seeded creative motion and pitch-drift delay, tempo clock, scale quantizer/MIDI tracker, worker-side pitch detector, harmonic calibration map.
- `cd_state`: scene/session documents with save pre-validation, SPSC transaction queue, generation pool, snapshot exchange, A/B mixer, 20-step history.
- `cd_blueprint`: blueprint parser/compiler to immutable `PreparedGraph`, lifecycle timeline, fault overlays.
- `cd_runtime`: `Engine` = oversampled MNA circuit + aligned dry + output stage + containment, latency planning.
- `cd_community`: SHA-256, catalog client, LRU cache against an abstract transport (mock only).
- Tests: one ctest suite per `tests/test_*.cpp`; RtTrap allocation trap in `tests/support`.

The product is incomplete: modules are not yet composed into one processing graph; no JUCE wrapper, GUI, host-qualified plugin, release binary, calibrated hardware model or measured CPU/memory compliance. See traceability for findings.

## Resume packets (PR #9, updated)

Writers this checkpoint used disjoint files per module; the coordinator owns CMake, docs and integration. Agree a writer before editing another module.

1. **P17 performance** (in progress): callgrind-guided optimization of MnaSolver/oversampler hot paths with ≤1e-12 output equivalence; re-run `cd_bench`; resolve the mix-0 dry deviation (disabled soft-clip should be a pure equal-latency delay).
2. **Engine taps**: post-input-trim tap and rail input for `cd_network`; Engine publishes `cd_analysis` TelemetryFrames; PitchDriftDelay state capture so snapshots work with pitch delay on.
3. **View-model wiring** (in progress): gestures/history → TransactionQueue, telemetry binding, UX-08 labels.
4. **Owner decisions needed**: G09 JUCE 8 pin + licence (GPLv3 vs commercial) and CLAP adapter revision to start P01/P08/P18 wrappers; G11 renderer; G07 encoder SDKs; G06 provider; G08 AAX access.
5. **Calibration (G01)**: real device manifests, reference schematics and independent curves replace every synthetic fixture before any physical claim.

## Verification and gates

Preferred build (add `-G Ninja` if available; set `ASAN_OPTIONS=detect_leaks=0` only where LSan cannot run):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCD_SANITIZE=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Read exact executed commands and failures in the integration review; do not copy a prior green result as a new run. The documentation validator checks documents, not audio or host behavior.

G01 needs actual devices/reference schematics and independent calibration. G03/G04 need measured solver/filter and CPU/memory budgets. G06/G07/G08/G09 need owner-led provider/encoder/AAX/licensing and dependency choices. G12 remains open for full checkpoint/model migration. No paid provider, credentials, security configuration, release or deployment was changed for this integration.
