# Handoff: reviewed core integration

Read [CLAUDE.md](../CLAUDE.md), [CHECKPOINT.md](CHECKPOINT.md), [INTEGRATION_REVIEW.md](INTEGRATION_REVIEW.md), [TRACEABILITY.md](TRACEABILITY.md), then the [implementation plan](../tasks/FEATURE_IMPLEMENTATION_PLAN.md).

## Source and ownership

This integration combines main `c10addecb48849d64890864f65f6ae231869be53`, PR #4 `a9c633fd89585a61780f206e303fced1c79326d1`, and the refreshed handoff from PR #2 `c3b73d95b117b979be0c761573c49046a3df6e9e`. These are the revisions inspected; recheck live refs before further work. PR #1 and PR #3 are already merged. Claude's original branches remain preserved.

Codex is integration owner for the user-approved reconciliation and safety fixes. During this change the writer boundaries were core JSON/state, oversampling, solver, and coordinator-owned CMake/tests/docs. There is no blanket claim of ownership over every unimplemented module. Agree a writer handoff before starting independent solver, state or instrumentation work on another branch.

## What exists

- `cd_core`: stable 74-parameter registry and bounded JSON parsing/writing.
- `cd_dsp`: thermal/protection primitives, original fixed 77-tap `Halfband2x`, configurable `PolyphaseHalfband2x` FIR/IIR cascade, synthetic device manifests and MNA solver.
- `cd_state`: scene/session documents, migration mechanism, compatibility decisions and local save helper. These are worker-side primitives; there is no integrated audio state transaction queue.
- Acceptance and safety regression executables, with verification limits recorded in the integration review.

The product is incomplete. No JUCE wrapper, GUI, host-qualified plugin, release binary, calibrated hardware model, cross-platform runtime qualification or measured CPU/memory compliance is claimed. The T21 hot 4x synthetic stimulus misses its proposed target; see traceability.

## Resume packets

1. **P04 queue/snapshot interface**: resolve provider-owned runtime layout and generations with the solver owner. Define producer/consumer ownership, coherent configuration revision, bounded preallocated request/publish slots, sample index/clock remainder, off-audio reclamation and stopped-audio save fallback before implementation. Acceptance: partitioned blocks including zero frames, queue pressure, concurrent save/restore, old-generation rejection and no audio allocation/lock/I/O. Configuration persistence must work when a dynamic checkpoint is unavailable. Do not replace the existing registry or JSON implementation.
2. **UX-09 instrumentation**: agree the process-entry contract and platform trap scope. A test-only thread-local callback scope may instrument allocation/deallocation, mutex/OS I/O and count violations without callback logging. Prove traps with positive controls and prove worker activity is excluded. Primitive smoke tests alone cannot pass the required one-hour integrated audio stress test.
3. **P03 integration**: place the current MNA solver inside the current oversampler; preserve thermal and electrical timebases across block partitions. Implement finite-output containment and the specified delayed-dry fallback. Re-measure aliasing on the circuit before claiming T21, then add latency padding/compensation and transitions. Keep the hot-case miss visible; do not lower its threshold or silently change quality.
4. **Publication**: current export validation supports only the explicitly bounded synthetic scene subset. Nonempty opaque macros/timeline/faults or unsupported blueprint payloads are rejected until P05/P10/P11 provide safe typed schemas. No cloud service is provisioned and no scene is uploaded by these helpers.

## Verification and gates

Preferred build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCD_SANITIZE=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Read exact executed commands and failures in the integration review; do not copy a prior green result as a new run. The documentation validator checks documents, not audio or host behavior.

G01 needs actual devices/reference schematics and independent calibration. G03/G04 need measured solver/filter and CPU/memory budgets. G06/G07/G08/G09 need owner-led provider/encoder/AAX/licensing and dependency choices. G12 remains open for full checkpoint/model migration. No paid provider, credentials, security configuration, release or deployment was changed for this integration.
