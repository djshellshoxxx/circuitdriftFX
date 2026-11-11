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

## Resume packets (updated by checkpoint on PR #9)

Module writers this checkpoint (disjoint files; coordinator owns CMake/docs/integration): control (Macros/Dynamics/CreativeMotion), musical (TempoClock/ScaleQuantizer/PitchDetector/HarmonicTarget), state (RtQueue/TransactionQueue/SnapshotExchange/AbSlots/History), runtime (src/runtime, tests/support/RtTrap), blueprint (src/blueprint), community (src/community), repair (StateDocument/ThermalNetwork additive). Agree a writer before editing another module.

1. **Wire the engine**: Engine consumes TransactionQueue events (sample-accurate parameter changes), GenerationPool-installed blueprint `PreparedGraph`s with 20 ms graph crossfade, MacroSmoother/evaluate effective values, timeline/fault overlays, AbMixer over two Engine paths, and SnapshotExchange capture with a provider layout descriptor. Keep all audio paths under RtTrap with positive controls.
2. **Temperature feedback**: MnaSolver needs a bounded parameter-update API and manifests need temperature laws before thermal state can affect the circuit (G01).
3. **Quality transitions**: live quality change with 20 ms crossfade and the host latency handshake; phase-family changes staged to deactivation.
4. **P12 LITE**: edition manifest + two-knob mapping (liteMappingVersion=1) over the same Engine; T05 peak ≤1e-6 versus the equivalent full configuration.
5. **UX-09 completion**: mutex/OS-I/O traps and the one-hour integrated stress.
6. **Adapters/UI (P08/P09/P18)** need G09 (JUCE pin/licence) and G11 decisions from the owner.

## Verification and gates

Preferred build (add `-G Ninja` if available; set `ASAN_OPTIONS=detect_leaks=0` only where LSan cannot run):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCD_SANITIZE=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Read exact executed commands and failures in the integration review; do not copy a prior green result as a new run. The documentation validator checks documents, not audio or host behavior.

G01 needs actual devices/reference schematics and independent calibration. G03/G04 need measured solver/filter and CPU/memory budgets. G06/G07/G08/G09 need owner-led provider/encoder/AAX/licensing and dependency choices. G12 remains open for full checkpoint/model migration. No paid provider, credentials, security configuration, release or deployment was changed for this integration.
