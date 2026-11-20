# Consolidation and fresh reviews — rounds 14–18, 2026-11-20

## Sources

Resumed PR #16 at b3e05b652147a584ad5fb22e5715d1e61b702318 and accumulated children through PR #25 at c6ebb22daab9c46edffa761b21f3fd45320e5074. PR #16 passed exact-source Linux and Windows/pluginval in run 44690495993; this does not qualify new changes. First combined local checkpoint: 1bfc6f89c8849cf3d998ca01fa31016eb8f7de14.

## Completed first fresh worker set

| Worker | Disposition | Executed evidence |
| --- | --- | --- |
| Code audit r14 | Stale preset selection fixed in 85714b4 -> bd378a6; scan bound handed to root; no detector finding. | Eight new assertions failed without the six-line fix, 15.24 s; fixed native panels passed 1/1, 14.25 s total. Sanitized pitch passed 1/1, 4.45 s. |
| Bug hunt r14 | Restore kind/identity guard 097f538 -> 8f97dc5; pre-copy JSON bounds, publication uniqueness and UTF-8 identity repair 146f248 -> 1bfc6f8. | Initial restore failed 12 checks/six cases. Rebuilt acceptance_core, state_safety and state_preset passed 3/3, 9.37 s. |
| Defensive r14 | No atomic destination-clobber defect within trusted-directory scope. Preflight/restore gaps repaired. Best-effort POSIX post-install cleanup explicitly documented. | Read-only source review. |

The code auditor independently cross-checked writer depth and byte accounting. State tests used GCC14.2 ASan/UBSan/float-cast-overflow, leak detection disabled. Production and most tests used O1; the large state_safety translation unit used O0 after a resource kill. A stale-executable run was discarded. Native panels used Clang19.1.7 O0 and pinned JUCE/CLAP, offscreen. Acceptance limits and assertions were retained.

## Feature work and successor reviews

333760c connects real blueprint compilation through a background worker with cancellation, one pending request and generation-checked reports. It exposes analytic operation counts and synthetic fixture-v1 provenance. Validation preview works; live graph Apply remains unavailable pending runtime installation/crossfade/retirement/undo ownership.

Catalog scans bound all visited entries and aggregate admitted reads, including failed partial reads. Searches filter cached snapshots; Refresh/import/delete invalidate appropriately. Fresh bug hunt r15 moved initial/manual scans off the UI thread. Fresh code audit r15 repaired the post-delivery report freshness issue and clears old results when text changes. Both successors completed and handed their work to fresh round-16 reviewers.

Fresh defensive review r15 completed on 1bfc6f8 with no new actionable finding. It independently checked every copied payload, inclusive depth/byte limits, inactive storage, UTF-8 identities, publication failure atomicity and file-install boundaries. This was source inspection and diff checking only, with no executed tests or edits.

Full dynamic session capture/restore, sound-preset load/save, dynamic A/B, graph Apply, faults/restoration, GIF/MP4, community/provider services, physical references and real host/performance qualification remain open. Original source branches and PR #9/#10/#14 semantics remain preserved.

## Combined-source evidence

At source 1bfc6f89c8849cf3d998ca01fa31016eb8f7de14, the live bridge and panels native suites passed 2/2 in 20.11 s (1.96 s and 18.13 s). This includes real background compilation, synthetic/cost provenance, unchanged saved sound, one-shot report consumption and pending session invalidation. Source 1d3e413 subsequently repairs retained TextEditor glyph colors after theme changes.

The combined sanitized core passed **31/31 in 333.79 s** at 1bfc6f8; both native suites passed as above. Exact case timings and flags are in `docs/evidence/continuation-round14-2026-11-20.json`. Later successor changes require their own targeted and exact-source CI qualification.

## Asynchronous catalog successor

Fresh bug hunt r15 completed c9fdf90a8ff67ceb68ccf0d58bef66ddd95e575e, integrated as a2a4a31. Initial/manual scans now use one lazy worker and one pending request. Superseded generations cannot replace the catalog. The panel polls completion, exposes loading/error/truncation, retains rows while loading and clears selection when new rows arrive. Query filtering does no filesystem work. Successful import/delete requests a fresh generation while retaining existing file safeguards.

Rebuilt sanitized state_preset passed 1/1 in 2.59 s on the worker commit. Tests hold an active provider before superseding it and check off-caller execution, coalescing, stale-result rejection, shutdown cancellation, error/retry and actual-folder budgets. These are synchronization checks, not an OS-read deadline guarantee. The native regressions passed in the later combined run below.

PR #25 exact head c6ebb22 completed Linux and Windows, including pluginval, successfully in run 44764678463. Its three detector production/test files are byte-identical to PR #17 head d12fd60d. This qualifies the incoming child stack, not later continuation commits.

## Validation freshness successor

Code auditor r15 completed 431a51b0423966941e880ccef8e06e139c469cab, integrated as 2bad4268499b8de993bd504f48b934e2f107a403. A delivered Valid/Ready report is now invalidated when the draft, sample rate, active oversampling, processor session epoch or audio-preparation state changes. The editable draft survives and can be revalidated. Live graph Apply remains disabled.

With unchanged 1bfc6f8 production and the new regression, ui_snap_live_bridge failed exactly five assertions in 1.32 s. With repaired production and identical test source it passed 1/1 in 1.56 s (1.57 s total). Two compiler-process interruptions returned 255 without a compiler diagnostic; rebuilding unchanged source with the cache launcher disabled succeeded. No assertions or build requirements were relaxed. See [the detailed report](BLUEPRINT_VALIDATION_FRESHNESS_2026-11-20.md).

The integration owner then rebuilt both affected native targets at combined source **2bad4268499b8de993bd504f48b934e2f107a403**, tree **b4c5a24191d123698010326372a7a198c3f2ec8e**, with Clang 19.1.7, Custom -O0, pinned JUCE/CLAP and no sanitizers. Both passed: **ui_snap_live_bridge 1.42 s; ui_snap_panels 11.51 s; 2/2 in 12.97 s**. Dark Blueprint and narrow light Presets snapshots were inspected. These are offscreen native applications, not interactive DAW or production-performance qualification.

## Third fresh worker set

- **Code audit r16**, pinned 2bad426: no new actionable integration finding. Reviewed status defaults and consumers, worker ownership/publication, snapshot-generation selection handling, retained theme text and C++20/thread linkage. Scoped diff check passed; no builds or edits.
- **Defensive security r16**, pinned a2a4a31: no new actionable finding in the two background services. Reviewed request admission, cancellation, bounded scanning, failed-read accounting, generation publication, ownership and destruction, and live bridge integration. The already assigned delivered-report freshness issue was excluded. Source/diff inspection only; no executed security tests or network activity.
- **Bug hunt r16**, pinned 2bad426: found that JUCE undo/redo actions retain their original glyph colors. A later undo/redo after a theme switch can restore low-contrast text. Repair 125b21732c76c865eb5cc5ae7e80384273aa7a06 was integrated as c73d6b9b9ebe6b22d03e2b72fd6b9d1b40bc97bb. A separate TextEditor listener reapplies current glyph colors without replacing callbacks or modifying text/history/caret/selection; SafePointer registrations detach on destruction or reparenting. Placeholder colors also follow the theme. The unchanged 2bad production plus the new regression failed **nine assertions in 11.03 s**: four placeholders, four undo/redo glyph cases and one reparented editor. The repaired source is covered by the final merged native run below.

The filesystem boundary remains a trusted directory. Already-running OS I/O cannot be preempted, so cancellation and joining do not establish a wall-clock shutdown deadline. Parsing/DC preparation are bounded work between cancellation checks. No ThreadSanitizer, leak, realtime deadline, physical-model or whole-repository security qualification is inferred.

## Publication and continuation

[The source-to-publication mapping](../evidence/commit-map-continuation-r14-2026-11-20.json) retains individual repairs. Every published source commit is constructed from committed UTF-8 file content and its full tree SHA is compared to the local tree before the branch ref is advanced. PR #16 remains the coordinator; main and source branches are preserved. Incoming child PR #17's detector files and audit are included through #19; #18–#25 are ancestral to c6ebb22. Historical instructions to hold isolated child refs are previous dispositions, not current integration blockers after their recorded CI completed.

Read the live PR head and checks before each continuation. Exact-head CI for this new source remains required; older green runs qualify only their own commits. Follow-up work should address the remaining features with bounded changes, fresh reviewers and concrete regression evidence. Credit balance is not observable and is not a qualification or stopping claim.

## Concurrent continuation and fourth fresh set

During final local verification, a concurrent continuation advanced PR #16 first to c6ebb22 and then to **02a7331cc687e710f3a3c7c0ab023e651601685e**. Its production source was e2d91a4 and its independent records report 30/30 sanitized core and 6/6 native results. Those records and their original source hashes are retained. The new exact-head run 44774625310 was queued when observed; it does not qualify this later merge.

The root preserved both complete histories in merge **9630ae03b4be4bc74b1d01d6bbf42b5259d8faec**, with parents local c73d6b9 and remote 02a7331. It retained the portable validation worker and asynchronous catalog as the initial source baseline, then integrated the unique remote contracts through reviewed follow-up commits. The intermediate merge was not presented as a qualified final source.

| Fresh r17 worker | Reconciliation and handoff |
| --- | --- |
| Code audit | Kept one portable validator and delivered-report freshness. Worker d6d580ed109e3f06cb9a3087bcabfe0b98bcefec, integrated fe961df, imports compiler cancellation before parsing, between stages, around probe setup/samples and before success; adds explicit cancel-generation/token gating, LITE and oversized pre-conversion rejection, unprepared-preview labeling, structured counts/Fs/OS/storage/provenance/diagnostics, Example RC and guarded delayed Apply callbacks. Both remote Blueprint regression headers and compiler cancellation assertions are preserved. |
| Bug hunt | Worker 397d090e83a91532aaef5ea51056d762be0c1a10, integrated 0849f8d, imports complete-envelope admission before copies and publication traversal, retaining local session-kind/UTF-8/parameter-count guards and all boundary tests. Worker 8d9075cf55a1e9a2654f77c78f6b075f01dbefd2, integrated 194e529, preserves selection by ID for query-only changes, uses the explicit double-click ID and invalidates selection for a new accepted generation or manual Refresh. Adds the remote selection cases and bounded-directory cases. |
| Defensive security | Independently identified those merge requirements and confirmed atomic writer and preset file-operation implementations were byte-identical across both parents. No competing file-install implementation was needed. Source equality, diff and regression-source inspection only; no execution. |

The root added the missing Blueprint panel-test invocation in **4dcd584f995c261b189774ea8f416fda40ad97a0**. All state/Blueprint/preset worker patches were inspected and committed in isolated worktrees; the workers did not build or execute tests. Final dynamic qualification belongs to the combined source below.

## Fifth fresh set and final source review

All three r18 workers reviewed **4dcd584f995c261b189774ea8f416fda40ad97a0** read-only:

- **Code audit:** no new actionable defect in the live validation path. Confirmed worker lifetime, admission before UTF-8 conversion, explicit cancel/publication checks, preview/current configuration identity, delivered-report invalidation and panel cancellation. Before enabling live graph Apply, capture and recheck Fs/OS/session epoch at delayed confirmation/dispatch; Apply is currently unavailable.
- **Bug hunt:** no new production defect in merged preset selection or theme listener paths. Found a test-proof gap: the new-snapshot query case advanced its generation before listPresets instead of during it. Root test-only commit **8ff0adfea571e977adec128622209f63bf997d1d** adds one-shot publication inside listPresets, exercising the actual status-after-list ordering contract.
- **Defensive security:** no new actionable finding. Confirmed complete-envelope admission, private-field publication stripping/order, strict checkpoint identities, catalog budgets, cancel-generation/token checks and listener ownership. Exact-file comparisons confirm the selected compiler files/test match 02a7331 and the catalog worker/library match c73d6b9.

Each review used source inspection and scoped diff validation. No reviewer claimed executed dynamic qualification. Root separately owns the sanitized core and all six native processor/editor applications. The post-review change is confined to the preset regression header; production source is unchanged.

## Final merged execution

Current production/review source: **4dcd584f995c261b189774ea8f416fda40ad97a0**. Current test-strengthening source: **8ff0adfea571e977adec128622209f63bf997d1d**, tree **476353329e03b5b9657af170cf735ac9ddc00163**. The full sanitized core passed **31/31 in 211.91 s**. All six native applications linked and passed **6/6 in 54.20 s** (processor 5.35 s, contract 1.31 s, live bridge 2.22 s, panels 12.29 s, shell 20.73 s, workbench 12.28 s). The full core run began at 4dcd584; the later header changes only native preset regression source and leaves all portable inputs unchanged.

The separate GIF branch `codex/cdfx-gif-capture-20261120-r14` remains preserved. Its observed head advanced to **ecae510b72c0ef4327bacfb6e4aa35d08eea2748** during this reconciliation. This audit has not qualified that moving branch. Three fresh R19 reviewers completed a separate [GIF preintegration review](GIF_PREINTEGRATION_REVIEW_2026-11-20_R19.md); fresh R20 backend, UI and resource workers own the resulting repairs. Resume their handoff before creating a duplicate capture implementation.

PR #17–#25 are now closed after source-inclusion and live-head verification. All nine original branch heads remain intact and main is unchanged. The containing publication still requires its own Linux/Windows/pluginval result.
