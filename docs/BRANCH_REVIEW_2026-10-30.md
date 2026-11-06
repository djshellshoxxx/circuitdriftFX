# Branch and handoff review after PR #5

Reviewed on 2026-10-30 after the owner's specific approval to merge PR #5.

## Verified merge and branch state

PR #5 merged as **`404317a266e887207137f67e67d5d92056d6c8d3`**. Its tree is **`437f3b9cb518186306edc7ddfa1e3b4af2f7ad40`**, identical to the tested integration head `2d048fe741f4bfa855e96eae2f60881c20276b52`. The merge used GitHub's expected-head check. No branch was deleted or force-pushed.

| Retained branch | Reviewed head | Ahead of merged main | Behind | Assessment |
|---|---|---:|---:|---|
| claude/circuitdriftfx-implementation-dknarc | `c3b73d95b117b979be0c761573c49046a3df6e9e` | 0 | 7 | Fully included; PR #2 merged/closed |
| claude/funny-meitner-drdlot | `7f395066f48a49da675cf36ab7461c554ef10d4e` | 0 | 7 | Fully included; PR #3 merged/closed |
| claude/zen-cannon-v5mb75 | `a9c633fd89585a61780f206e303fced1c79326d1` | 0 | 5 | Fully included; PR #4 merged/closed |
| codex/reconcile-core-safety-20261030 | `2d048fe741f4bfa855e96eae2f60881c20276b52` | 0 | 1 | Fully included; PR #5 merged/closed |

PR #1 is also merged/closed. No implementation branch had unmerged commits at this review. This does not establish the absence of unpublished work in an external session. Recheck live refs before new edits; later documentation branches are not covered by this snapshot.

GitHub's [merged-main Linux run 41036150506](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/41036150506) passed Release CMake configure/build and **7/7 CTest entries**: original acceptance, state safety, oversampling safety, solver safety, primitive safety, registry freshness and documentation validation. This is separate from [integration-head run 41034961456](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/41034961456), which also passed 7/7. No new code test was inferred from the documentation review.

## Handoff findings

1. The checkpoint/handoff still named the pre-merge integration branch and historical input SHAs. Promote merged main to the restart baseline while preserving those inputs as review history.
2. The handoff listed P04 queue/snapshots first even though P04 depends on P03 runtime ownership, clocks and layout. Prioritize a bounded synthetic MNA-inside-oversampler process slice, then queue/snapshot publication.
3. `MnaSolver` retains borrowed manifest pointers. Preparation/installation must establish immutable ownership before asynchronous replacement or queues; static catalogue lifetime does not solve dynamic-import lifetime.
4. Failure recovery needs processor-owned latching. The solver's consecutive-failure flag clears on a successful step; it is not the specified persistent fallback state. Containment must address filter history as well as final output.
5. Current FIR latency is fractional and IIR latency is frequency-dependent. Delayed-dry, mix/bypass and fallback claims require verified compensation/alignment first.
6. State-envelope validation is not full blueprint validation or compilation. The minimal export fixture is not an executable circuit. Current checkpoint compatibility also omits complete topology/filter/RNG/clock/A-B identity and history.
7. Old planning prose and environment limitations can mislead a restart. Keep specifications as requirements, current code/traceability as progress evidence, and recheck current tools rather than treating a previous missing CMake/proxy failure as permanent.

Two independent read-only handoff reviews agreed on P03 integration before dependent P04 work. Their review produced the [consolidated continuation prompt](prompts/CONTINUE_DEVELOPMENT.md). No runtime feature was implemented by this follow-up documentation change.

## Post-merge validation findings

Qodo submitted a [ten-item review](https://github.com/djshellshoxxx/circuitdriftFX/pull/5#issuecomment-6519873632) after the approved merge. Source review confirms the following gaps in the merged tree. They remain open and take priority over new feature implementation. CodeRabbit reported that its review failed because the PR was closed; no clean automated review is claimed.

| Finding | Source boundary | Required follow-up |
|---|---|---|
| Manifest identity accepts decimal tokens rounded to integer doubles | `DeviceManifest.cpp:59–65` | Require exact-integer provenance for schema/revision; test fractional-token aliases |
| Manifest schema accepts cases rejected by runtime | `device-manifest.schema.json`, `DeviceManifest.cpp:103–127` | Align diode-only domain, strictly positive exponent limit and measured-source conditions; extend the restricted schema checker and test both accepted/rejected fixtures |
| Writer emits malformed in-memory UTF-8 that parser rejects | `Json.cpp:214–223` | Validate string values and object keys on output |
| Creation-time validator accepts impossible dates/times | `StateDocument.cpp:49–60` | Validate calendar and clock ranges, with explicit leap-second policy |
| Restore compatibility omits RNG state count | `StateDocument.h:87–94`, `StateDocument.cpp:428–439` | Require the runtime RNG layout; retain the broader integrated-checkpoint limitations |
| Ordinary save can emit a document its loader rejects | `StateDocument.cpp:239–267,343` | Validate the entire in-memory envelope before file replacement, including scene/checkpoint, blueprint and parameter constraints |
| In-memory manifest bounds/scales can accept infinity | `DeviceManifest.cpp:44,91–101` | Reject non-finite values before solver preparation |
| Metadata assembly performs repeated linear key searches | `Json.cpp:16–23`, `StateDocument.cpp:266` | Validate unique non-reserved metadata first, then use bounded assembly and measured performance evidence |
| Thermal step rejection has no visible status/counter | `ThermalNetwork.cpp:100–118`, `ThermalNetwork.h` | Add diagnostics before thermal coupling, preserving no-change-on-rejection semantics |

The review also observed that POSIX atomic replacement installs a mode-0600 temporary inode. This is an intentional private-file default with a shared-file compatibility tradeoff; permission-preserving behavior needs an explicit policy. Do not blindly apply the bot's suggestion to widen new files, follow symlinks, or read umask by temporarily changing a process-global setting. No permissions were changed during this review.

Public in-memory `StateDocument` callers can also supply reserved metadata keys: the current `set` loop can overwrite canonical fields. A metadata optimization must reject those collisions rather than assuming every document came from `loadState`. A reload-only check also cannot detect a semantically valid overwritten field, so validate that boundary explicitly.

### Executed bounded reproductions

Two independent reviewers ran temporary harnesses against the unchanged merged source snapshot:

- GCC 14.2 state/JSON harness (`-std=c++20 -O2`): malformed UTF-8 was emitted then rejected on parse; `2026-02-30T25:61:61Z` loaded; saved scene-plus-checkpoint, empty-blueprint and out-of-range-parameter documents failed reload. Atomic save of an invalid empty-blueprint document replaced a dummy destination successfully but produced an unreadable document. Reserved metadata changed seed 42 to 99 with a successful reload. RNG lengths 0, 2 and 64 all returned `Restore` against the same runtime identity.
- The same harness observed accepted metadata sizes 2,000/4,000/8,000 entries taking approximately 6/22/164 ms to save in one run. This supports the source-level quadratic-growth concern; it is not a controlled benchmark or product CPU qualification. Replacing a dummy mode-0640 file produced mode 0600, confirming the permission-policy tradeoff.
- GCC manifest/thermal harness with ASan/UBSan (`ASAN_OPTIONS=detect_leaks=0` for this sandbox's ptrace limitation): fractional schema/revision tokens rounded to 1 had `numberIsExactInteger=false` but were accepted. Infinite scale, infinite upper bound and all-infinite bounds/defaults were accepted from in-memory values. A finite-power overflow retained the thermal state correctly, with no API diagnostic available.
- The repository's schema-checking function accepted four fixtures rejected by runtime: diode without domain, resistor with domain, measured provenance without source and zero `maxExpArg`.

The harnesses exited successfully while exhibiting these defects; exit zero here is diagnostic evidence, not a passing regression fix. Local artifacts are `/tmp/circuitdriftfx-late-state-review/repro.cpp` and `/tmp/circuitdriftfx-qodo-triage/manifest_thermal.cpp`; copies are retained in the session evidence directory. No repository runtime source changed. Documentation validation passed all mappings/schema-example checks and 95 local Markdown links; those schema-example checks do not cover the mismatches above.

These findings do not invalidate the recorded 7/7 result; they show missing coverage. Fix them in a separate regression-backed increment before P03. This branch/handoff review is not the requested eventual whole-codebase bug audit or deep security audit; both remain in the continuation prompt.

## Continuing limits

The hot synthetic 4x alias result remains −53.13 dB against a −70 dB target. Physical calibration, full realtime stress, CPU/memory targets, Windows/macOS/host behavior, wrappers/UI, full A/B/state capture and owner-dependent access/licensing remain open. Scene publication remains restricted to a minimal validated synthetic subset. Refresh review comments again at the start of the next implementation session.
