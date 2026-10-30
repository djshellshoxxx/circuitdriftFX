# CircuitDrift consolidated development prompt

Copy the text below into the next coding session with this repository attached. It requests implementation, not another planning-only handoff. Recheck live evidence before acting; the supplied baseline is a dated observation.

---

You are the integration coordinator for **djshellshoxxx/circuitdriftFX**. Continue autonomous development from the latest reviewed main. Reuse the existing code, coordinate useful workers with disjoint ownership, implement the next dependency-ready slice, verify it, and publish a meaningful draft PR with exact evidence. Preserve the full product scope: CD-F03–CD-F25 and UX-01–UX-10 remain required; partial milestones do not remove later features.

## 1. Establish current truth before writing

The verified merged baseline on 2026-10-30 is main **`404317a266e887207137f67e67d5d92056d6c8d3`**, tree **`437f3b9cb518186306edc7ddfa1e3b4af2f7ad40`**. PR #5 merged integration head `2d048fe741f4bfa855e96eae2f60881c20276b52` after the owner's specific approval. PRs #1–#5 are all merged/closed. All four retained non-main branch heads had zero commits ahead of this main; original branches were preserved. See `docs/BRANCH_REVIEW_2026-10-30.md` for exact comparisons. Do not remerge those historical branches or restart their implementations.

Refresh GitHub branches, open/closed PRs, checks, review comments and any newer handoffs. Check local status, worktrees and unpublished changes before switching. Create an isolated branch/worktree from the latest appropriate main; no resets, force pushes or removal of another writer's work. Local directories named `*-review-*` or `*-fixes-*` may be source snapshots without Git history: verify their provenance instead of treating a directory name as a branch.

Read applicable `AGENTS.md` and relevant `.agents/skills` if present, then `CLAUDE.md`, `docs/CHECKPOINT.md`, `docs/HANDOFF.md`, `docs/TRACEABILITY.md`, the branch/integration review, `tasks/FEATURE_IMPLEMENTATION_PLAN.md`, and the relevant specifications and acceptance tests. No AGENTS.md or repository skill files were found at the recorded baseline; check again. The original `ENGINE_SPECIFICATION_PROMPT.md` is a historical specification-writing assignment, not an instruction to recreate the scaffold or avoid the implementation requested here. Older planning-status prose is not evidence that current code is absent.

In a managed cloud environment, inspect its supported readiness/network status and runtime instructions. The previous session's Git proxy was unavailable and local CMake was absent; GitHub connector operations and GCC worked. Recheck those capabilities now. Preserve injected credentials/proxies, do not bypass restrictions, and use supported connectors when sufficient. Record any actual transport blocker precisely while continuing independent work.

## 2. Preserve the implemented baseline

- `cd_core`: immutable 74-parameter registry and generated IDs; bounded UTF-8 JSON parser/writer, locale-independent numbers and exact-integer provenance. Do not regenerate or reassign permanent IDs to suit new graphs.
- `cd_dsp`: `ThermalNetwork`, `ButterworthHpf`, `softClip`, `GainMatcher`, main's fixed 77-tap `Halfband2x`, configurable `PolyphaseHalfband2x`/`HalfbandIir2x` through `Oversampler`, synthetic device manifests and `MnaSolver`.
- `cd_state`: worker-side scene/session documents, strict identity/model checks, migration mechanism, compatibility decisions, constrained scene export and exclusive temporary-file save. `updateMeanSquares` is the linked-channel gain API; `update` takes sample amplitudes.
- Existing acceptance tests plus state, oversampling, solver and primitive safety suites. Keep their regressions and both halfband APIs unless a separately justified migration preserves behavior.

These are primitives, not a complete plugin. There are no qualified wrappers, GUI, integrated thermal/electrical engine, full A/B/history, audio transaction queue, full blueprint compiler or production snapshot-capture pipeline. Only synthetic R/C/diode models exist; no hardware calibration is established.

## 3. Coordinate ownership

Remain the single integration owner. Before edits, record one canonical writer and file scope for each module. Resolve any active Claude or other worker ownership using current repository/session evidence; an old merged branch does not prove an external unpublished session is idle. Do not silently treat missing code as unowned. If overlap cannot be resolved, prepare a bounded interface/acceptance packet and continue disjoint tests/review while the coordinator obtains a handoff.

Use useful workers in parallel when their inputs are ready. A runtime writer should own process configuration, clocks, model lifetimes and recovery; a state writer can implement publication/queues after those provider contracts are agreed; an instrumentation/review worker can exercise the agreed callback boundary. The coordinator owns shared build files, integration evidence and reconciliation. Prefer independent review to concurrent edits of the same files. Use an appropriate stronger model for difficult numerical/security reasoning within available authorization; no paid provider setup. Paid Claude may only use the existing shared broker `01a17a22-35e0-7475-80ad-3d841affa039`, after verifying its remaining cumulative $200 ceiling and any stricter limits/reserve. If that cannot be verified, continue with available local workers rather than purchasing or bypassing the broker.

## 4. Close the confirmed validation gaps before extending the runtime

The post-merge review found remaining defects despite the existing 7/7 green suite. Read the triage in `docs/BRANCH_REVIEW_2026-10-30.md`, reproduce it on the latest main, and deliver a bounded validation-fix increment first. Do not treat automated review as a substitute for reproductions or copy its suggested patches without checking public in-memory API callers.

- Require exact integer provenance for device-manifest schema/revision and finite bounds/defaults/scales, including directly constructed `JsonValue` inputs. Align manifest schema and runtime conditions for diode domains, strictly positive exponent limits and measured provenance; extend the documentation validator when introducing schema keywords it does not currently support.
- Reject malformed UTF-8 in writer strings and object keys. Ensure ordinary state saving cannot succeed with text the loader rejects: validate the full in-memory envelope, scene/checkpoint rules, blueprint presence, parameter ranges, size/depth and metadata boundaries before replacing any destination. Reject reserved metadata-key collisions instead of allowing them to overwrite canonical state. Preserve valid caller data rather than silently dropping unsupported fields.
- Validate calendar/clock ranges in `createdAt`, and make the supported leap-second policy explicit. Add RNG-layout compatibility to restore decisions; do not imply this closes complete integrated checkpoint identity.
- Remove quadratic metadata assembly only after guaranteeing key uniqueness and separation from reserved fields. Measure representative bounded inputs; the automated review's approximate key count is not a measured performance result.
- Preserve atomic-save safety. Owner-only POSIX replacement permissions are current behavior; define a deliberate shared-file permission policy before changing them. Do not blindly widen permissions, follow destination symlinks, or mutate process-wide umask in a plugin.
- Add observable, bounded thermal-step rejection status/counters before thermal coupling; retain the invariant that rejected steps commit no temperature changes.

Test malformed in-memory values as well as parsed input, prove existing destination contents remain intact on validation failure, and publish exact regression evidence. This repair packet precedes P03; it does not replace the accepted feature plan.

## 5. Next feature priority: P03 synthetic process integration

Start with a bounded, framework-independent **mono, double-precision, fixed-quality synthetic RC/diode processor** that runs the existing `MnaSolver` inside the existing `Oversampler`. Define explicit source-input and output-voltage mappings with units and calibration for the synthetic fixture. Use a small provider-owned runtime layer rather than rebuilding solver/filter/state libraries. Record the contract, implement it, add meaningful tests, and deliver a passing increment; do not stop at an architecture outline.

Required contract decisions:

1. **Preparation and lifetime.** Allocate/design/solve initial conditions off-thread. `NetElement::model` is borrowed: retain an immutable owner for every referenced manifest, or copy the required model data during preparation. Never leave processing with pointers to a temporary parsed manifest. Prepare a candidate before installation. Failed `Oversampler::prepare` invalidates that object; failed solver preparation preserves its prior state. Do not apply either operation directly to an active callback object without a safe ownership contract. Reclaim replaced resources off the audio thread.
2. **Clocks and buffers.** Retain sample position across callbacks; zero-frame calls advance no audio or control clock. Support the specified prepared slice bound, split larger buffers without allocation, and test different block partitions. Keep process/reset APIs noexcept, bounded and free of allocation/deallocation, locks, file/network I/O and logging. Use double state. Make unsupported layouts/rates explicit instead of implying full host qualification.
3. **Failure behavior.** Prevent NaN/Inf input from poisoning persistent oversampler/HPF state; define diagnostics and recovery for stateful stages, not only final-output replacement. Never commit invalid solver state. The processor must latch fallback after the specified failure condition: `MnaSolver::needsFallback()` alone clears after a successful step and is not the required persistent recovery state. Re-entry needs explicit Reset/Apply with successfully prepared state. Implement the specified finite-output/absolute-8 containment and transition behavior when the aligned dry path is ready.
4. **Latency before dry-path claims.** Current 4x FIR round-trip latency is 79.5 base samples; 16x is 83.375. Define, implement and measure fractional compensation and the reported integer delay before claiming aligned bypass, blend or fallback. IIR latency is only nominal DC group delay. Keep quality fixed in the first prepared path; live quality transitions and host latency handshake are subsequent work. A first electrical-path commit may be partial, but complete the aligned containment/transition increment before claiming runtime integration acceptance.
5. **Thermal coupling.** Existing manifests do not provide electrical-to-thermal node mappings, a dissipative-power reporting contract or temperature feedback laws. Define those before coupling. Keep units explicit; do not pass raw audio amplitude as watts or count capacitor energy transfer as heat. Any interim power/thermal fixture must be explicitly synthetic. Preserve the fractional 1 kHz thermal clock when it is connected.

Acceptance for the slice must include independent linear RC/nonlinear references; block partitions `{1,17,64,127,512,8192}` with zero-frame calls; failed prepare and stale-generation handling; destroyed/replaced manifest lifetime under ASan; bounded buffer behavior; invalid-input containment and recovery; measured latency and timed fallback; and reset reproducibility. Add allocation/deallocation trap positive controls at the process boundary. A short primitive run cannot satisfy UX-09's one-hour allocation/lock/OS-I/O stress requirement.

Re-measure the integrated circuit's aliasing against the offline reference with documented stimuli and exclusions. Preserve the existing hot synthetic 4x miss: **−53.13 dB versus the proposed −70 dB target**. Nominal fixture success and per-stage filter rejection do not close T21. Do not weaken thresholds, silently increase the selected quality, or label the electrical model hardware-validated.

## 6. Then advance P04 and UX-09 from real provider contracts

After runtime ownership/layout/clock decisions are agreed, implement bounded parameter transactions and preallocated snapshot request/publication with explicit producer/consumer ownership, generations, queue-pressure policy and worker reclamation. Host save must return the last coherent published snapshot without waiting on stopped audio; configuration persistence remains mandatory when dynamic capture is unavailable.

Current `EngineCheckpoint`/`decideRestore` is a limited compatibility primitive. It does not establish topology identity, filter/delay histories, RNG layout, thermal-clock phase, fallback state, configuration generation or complete A/B continuity. The existing zero-error continuation result covers one synthetic MNA fixture. Define versioned provider layouts and complete capture before claiming exact integrated continuation. Test incompatible Fs/model/layout fallback, concurrent saves, overflow, stale generations, stopped audio and unchanged live configuration on rejection.

`loadState` validates a limited envelope; it is not a full blueprint compiler. The minimal publishable synthetic fixture lacks nodes/ports/pins/parameters required by the complete blueprint schema and must not be treated as a playable circuit. Introduce an explicit validated/compiled configuration boundary before Apply. Publication currently rejects unsupported opaque sound-bearing payloads; preserve that protection until P05/P10/P11 have typed semantic validators. Feature-ID recognition does not mean the feature is implemented; the v0 migration fixture is test-only.

After these foundations, follow the accepted P05–P18 dependency plan for macros, state/A-B/history, UI, blueprints, recorder, community, IPC and qualification. Do not fill gaps with duplicate scaffolds or claim the whole product complete from this first slice.

## 7. Verify, review, checkpoint and ship reviewable increments

First reproduce the baseline if the current environment supports it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCD_SANITIZE=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Historical evidence: integration-head CI run `41034961456` and merged-main CI run `41036150506` each passed 7/7 Linux tests. These are prior evidence, not a new session's result. Local GCC14 runs also passed, with LeakSanitizer disabled solely for the sandbox ptrace limitation. Recheck toolchain capabilities and record any exclusions; do not silently disable failing checks. The documentation validator validates documents/examples only. Name the exact tested SHA/diff, compiler, commands and fixture provenance; separate source review, executed tests and full qualification.

For each meaningful increment: run relevant tests, obtain independent code/bug review, examine the affected security boundary, fix confirmed findings, and maintain `CHECKPOINT.md`, `TRACEABILITY.md` and `HANDOFF.md`. Review incoming automated findings against source and reproductions rather than accepting or dismissing them blindly. Inspect workflow side effects before publishing. Push a normal branch and draft PR with scope, evidence and remaining gates; consolidate passing reviewed work while preserving authors' history. The specific approval to merge PR #5 is not blanket approval for future PR approvals/merges. Do not submit an approval review or merge a later PR without the required specific user confirmation.

After feasible implementation increments, perform the requested whole-codebase bug/code audit, then a deep security review. Cover parsers/import boundaries, persistence and data loss, publication privacy, resource exhaustion, model lifetimes and concurrency, realtime constraints, dependencies/licensing, build workflows, and future network/IPC boundaries when they exist. Keep a severity-ranked findings ledger with reproductions, fixes and residual risks; a passing suite or a branch review is not a completed audit.

Keep G01 physical references/calibration, G03/G04 solver/filter/resource budgets, G05 host IPC behavior, G06–G09 provider/encoder/AAX/JUCE-CLAP access/licensing, G10 pitch evidence, G11 renderer qualification and G12 full snapshot migration explicit. Continue allowed synthetic/core work while owner-dependent gates remain open. Do not provision paid services, change credentials/security settings, publish releases/deployments, or bypass denied actions. No external messages to people/services without authorization.

End each checkpoint with completed behavior, exact branch/commit/PR, tests actually run, unresolved findings, ownership and a concrete next action. Report blocked actions with target, reason, attempted result and unaffected work. Begin now by refreshing the repository evidence, assigning writers for the validation repair packet, and reproducing/fixing those defects; then establish the runtime writer and implement the bounded P03 slice.
