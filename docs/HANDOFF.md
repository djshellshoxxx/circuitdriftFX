# Handoff: reviewed core integration

Read [CLAUDE.md](../CLAUDE.md), [CHECKPOINT.md](CHECKPOINT.md), [INTEGRATION_REVIEW.md](INTEGRATION_REVIEW.md), [TRACEABILITY.md](TRACEABILITY.md), then the [implementation plan](../tasks/FEATURE_IMPLEMENTATION_PLAN.md).

## Source and ownership

This integration combines main `c10addecb48849d64890864f65f6ae231869be53`, PR #4 `a9c633fd89585a61780f206e303fced1c79326d1`, and the refreshed handoff from PR #2 `c3b73d95b117b979be0c761573c49046a3df6e9e`. These are the revisions inspected; recheck live refs before further work. PR #1 and PR #3 are already merged. Claude's original branches remain preserved.

Codex is integration owner for the user-approved reconciliation and safety fixes. During this change the writer boundaries were core JSON/state, oversampling, solver, and coordinator-owned CMake/tests/docs. There is no blanket claim of ownership over every unimplemented module. Agree a writer handoff before starting independent solver, state or instrumentation work on another branch.

## What exists

- `cd_core`: stable 74-parameter registry and bounded JSON parsing/writing (locale-independent, exact integer identities, UTF-8-validated output).
- `cd_dsp`: thermal network (with step status), protection primitives, fixed and configurable halfband/oversampling (4-output FIR block path), synthetic manifests, MNA solver (m≤8 specialisations, bounded `updateParameter` API).
- `cd_control`: macros, transient/sidechain detectors, seeded creative motion and pitch-drift delay (with snapshot state API), tempo clock, scale quantizer/MIDI tracker, worker-side pitch detector, harmonic calibration map.
- `cd_state`: scene/session documents with save pre-validation, SPSC transaction queue, generation pool, snapshot exchange, A/B mixer, 20-step history.
- `cd_blueprint`: blueprint parser/compiler to immutable `PreparedGraph`, lifecycle timeline, fault overlays.
- `cd_runtime`: `Engine` = oversampled MNA circuit + aligned dry + output stage + containment, latency planning, transaction/macro/graph/A-B/snapshot wiring, detectors, tempo clock, pitch-drift delay, fault channels, telemetry publication (snapshot layout v3).
- `cd_analysis`: telemetry frames and XY/rail/phase display math. `cd_media`: recorder core (PNG/GIF/WAV, A/V sync). `cd_uimodel`: framework-independent view-model (gestures, shell, sphere, tooltips). `cd_edition`: LITE edition over the same Engine.
- `cd_network`: protocol envelope, prepared SPSC rings, group broker, member consumer, bleed, SYNTHETIC shared PSU and deterministic co-render/influence bundle — in-process mocks only (G05).
- `cd_community`: SHA-256, catalog client, LRU cache against an abstract transport (mock only).
- Tests: one ctest suite per `tests/test_*.cpp`; RtTrap allocation/lock/I-O trap in `tests/support`; `tests/test_rt_stress.cpp` (5 s per PR, one hour nightly and executed locally); `benchmarks/bench_engine.cpp` with `benchmarks/README.md`.

The product is incomplete: no JUCE wrapper, GUI, host-qualified plugin, release binary, calibrated hardware model, real IPC transport or measured CPU/memory compliance on reference hardware. See traceability for findings.

## Resume packets (updated by checkpoint on PR #10)

Module writers this checkpoint (disjoint files; coordinator owns CMake/docs/integration): runtime wiring (src/runtime), performance (src/dsp internals, benchmarks), network (src/network, tests/test_network_*), faults-to-solver (src/runtime, in progress on `wt-faults`). Agree a writer before editing another module.

1. **Faults on graph paths**: the built-in circuit now receives fault parameters through `MnaSolver::updateParameter`; installed blueprint graph paths report a reason and get none because their element indices differ. Map blueprint component ids to graph solver elements and extend the fault tests to an installed graph. ESR/leakage halves stay unavailable until the fixtures carry those parameters.
2. **Network frame consumption**: Engine consumes `cd::network::NetworkFrame` in the per-sample order ("available network frame"): bleed into the pre-circuit input and shared rail fraction as a readout; Network-unavailable status fades to local with zero bleed. Requires the adapter to supply the host transport epoch. Real shared memory, broker process and the per-host matrix remain G05.
3. **Temperature feedback**: manifests need temperature laws (G01) before thermal state can drive `updateParameter`; the API is ready.
4. **Quality transitions**: live quality change with 20 ms crossfade and the host latency handshake; phase-family changes staged to deactivation.
5. **Performance on reference hardware**: rerun `cd_bench` on the listed CPUs (G03/G04); Eco and 1x targets are still missed on the shared VM. Further optimisation must keep bit-identity or document the numerical change.
6. **Adapters/UI (P08/P09/P18)** need G09 (JUCE pin/licence) and G11 decisions from the owner; MP4 needs G07.

## Verification and gates

Preferred build (add `-G Ninja` if available; set `ASAN_OPTIONS=detect_leaks=0` only where LSan cannot run):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCD_SANITIZE=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Read exact executed commands and failures in the integration review; do not copy a prior green result as a new run. The documentation validator checks documents, not audio or host behavior.

G01 needs actual devices/reference schematics and independent calibration. G03/G04 need measured solver/filter and CPU/memory budgets. G06/G07/G08/G09 need owner-led provider/encoder/AAX/licensing and dependency choices. G12 remains open for full checkpoint/model migration. No paid provider, credentials, security configuration, release or deployment was changed for this integration.
