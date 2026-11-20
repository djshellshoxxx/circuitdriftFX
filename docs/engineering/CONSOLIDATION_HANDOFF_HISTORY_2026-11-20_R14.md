# Historical consolidation handoff through parallel R14

This preserves the handoff published in 02a7331. Its pending-work and ownership statements are historical. Read [the current handoff](CONSOLIDATION_HANDOFF.md) before continuing.

# CircuitDriftFX consolidation handoff — 2026-11-20

## Current checkpoint — R14

R14 integrates the nine-branch continuation chain through
`c6ebb22daab9c46edffa761b21f3fd45320e5074`, repairs fresh preset/state defects, and
connects the Full edition Blueprint panel to real asynchronous compiler validation.
The separate pitch-preparation branch was already imported byte-identically.
Original refs and older PRs remain preserved. The
[integration audit](../audits/R14_INTEGRATION_REVIEW_2026-11-20.md) records the lineage.

Final production/test source is remote
`e2d91a45d6a45ab8987622a00a19fbed1049e7e6`, identical to local
`b78430cd941201be382a09809ca40be997e959c1`, tree
`8bad08aa4a5bca5b2ef796536cd1ceff3806d797`. The containing checkpoint adds evidence
and handoff documentation. The [R14 commit map](../evidence/commit-map-2026-11-20-r14.json)
preserves all eleven new commits with verified tree equality.

Draft [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16), branch
`codex/cdfx-consolidation-20261119`, remains the publication and CI authority.
Read its live head and checks: **R14 requires its own exact-head Linux/Windows and
pluginval run.** Historical child-branch ownership holds below are superseded by
this integrated checkpoint; those sections remain provenance.

| Evidence | Result and exact boundary |
| --- | --- |
| Combined portable core | **30/30 passed, 335.45 s**, GCC 14.2 `-O1`, ASan/UBSan/float-cast-overflow. The only later source change is Blueprint text recoloring; portable source/build inputs are identical. Leak detection was disabled under the tracer. |
| Final native processor/editor | **6/6 passed, 25.23 s**, Clang 19 Custom `-O0`, pinned JUCE, after the visual theme repair. Offscreen console applications, not DAW qualification. |
| Generated registry/plugin tables | **Passed** separately. |
| Previous PR16 source `b3e05b6` | **Linux, Windows and pluginval passed**, [run 44690495993](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993). |
| Inherited chain source `c6ebb22` | **Linux, Windows and pluginval passed**, [run 44764678463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44764678463). |
| New R14 remote qualification | Required on the containing publication. Earlier source results do not qualify new code. |

Read [the R14 validation record](VALIDATION_2026-11-20_R14.md) for source hashes,
commands, full logs, build recoveries, independent review scope and limits. The
[earlier validation record](VALIDATION_2026-11-20.md) remains historical evidence.

## Goal, authority and release scope

The owner requested consolidation of genuinely unique work, continuation of
incomplete features, separate code/bug/security workers, and fresh-worker
handoffs. Source integration and verified repairs are authorized. Preserve
existing branches and acceptance criteria. This development request does not
establish physical calibration, release qualification, or authority to provision
paid services. No release or deployment is part of this checkpoint.

Read this file, the live refs/PRs, [the source inventory](consolidation-state.json),
`CLAUDE.md`, ADR-0001, and the audit handoffs before selecting work. Coordinate
with active workers; repository state may advance after any saved prompt.

## Implemented feature and repair state

### R14 live validation and local file services

- Full-edition Blueprint drafts now use the real compiler on one cancellable
  worker. Reports identify synthetic models, actual Fs/OS, diagnostic locations,
  arithmetic estimates and storage estimates. Example RC creates an editable
  synthetic draft. Validation changes no audio, parameters, preferences or history.
- Replaced or cancelled jobs cannot publish stale results. Edits, reload, slot
  changes during validation and panel destruction cancel obsolete work; host-state
  or observed audio-configuration changes invalidate pending reports. Apply remains
  disabled because the runtime graph transaction is not implemented.
- Factory/user catalog lookup, search, import/export, user deletion and atomic
  PNG output are integrated. Complete sound preset load/save still requires
  blueprint/seed/runtime transactions. Scans count examined directory entries
  before filtering and surface partial/error status. Selection follows stable IDs.
- Scene publication rejects duplicate parameter identities. State writers admit
  retained JSON depth and encoded size before copying; publication skips private
  fields. Earlier checkpoint layout, cardinality, whole-envelope and exclusive
  preset-creation guards are retained. These are state API improvements, not full
  plugin dynamic-session persistence.

### Live editor, reset and host preferences

- The processor owns the production `ProcessorEditorBridge` and constructs
  `ui::CircuitDriftEditor`. Live APVTS gestures, bounded 20-step undo/redo,
  host-value conflict checks, A/B parameter copy/swap and mirrored crossfade are
  implemented. A validated host-state load invalidates stale local history.
- Explicit touched-parameter declarations keep unrelated host automation out of
  a user's undo step. Keyboard, wheel, numeric, reset and drag edits have matching
  host gesture boundaries, including destruction during a drag.
- Persistent processor-owned triple-buffer snapshots publish telemetry, quality
  and control status, effective component targets and tempo from the audio
  owner. UI reads no longer depend on runtime-owned memory surviving a
  prepare/release transition. The single message-thread reader contract and
  normal serialized host audio lifecycle still apply.
- Warm/Cold UI requests carry the prepared-runtime generation and are consumed
  by the audio owner on the next callback. Repeated requests coalesce. Full
  prepared processors expose reset; LITE and unprepared processors disable it.
  Runtime reset updates audio-owned state and invalidates pitch by epoch, leaving
  analysis-buffer reset to the worker. Existing quiescent reset semantics remain.
- Audio-key disable/re-enable, source changes and dropped input invalidate old
  pitch immediately and require fresh acquisition. Zero-frame processing
  publishes current reset/quality diagnostics without advancing audio clocks.
- Versioned optional UI fields in host XML persist scale, theme, editor view,
  educational/accessibility settings, overlays, X-ray, PCB view and dimensions.
  Bounds and type validation precede parameter replacement; legacy
  parameter-only states remain readable. Local preference changes notify the
  host of nonparameter state changes; identical writes and restores do not.
- An open editor synchronizes restored preferences before refresh, local edits,
  menu toggles and resize. Restoration avoids writeback/notification recursion.
  A newer explicit resize uses the restored scale and retains its newer physical
  dimensions. Workbench view buttons persist their projection; unrelated
  settings and reselecting a view preserve orbit/pan/zoom.

### State repairs

- Wheel-history coalescing stages a replacement and checks its byte budget before
  mutation. Rejection preserves the prior transaction; undo boundaries,
  intervening host values and non-finite timestamps are handled explicitly.
- Preset reads allocate and consume only the checked bounded size, rejecting
  short reads or appended data before publishing a result.
- A failed parameter-snapshot coherence read leaves the caller's last accepted
  snapshot unchanged.
- The subsequent ring/coalesced handoff repair transfers complete events through
  per-parameter SPSC mailboxes and rejects stale ring events using sequence
  numbers. Legitimate value decreases remain valid. Sequence exhaustion rejects
  explicitly, and callback work remains fixed and allocation-free.

### Commit and audit map

| Integrated commit | Change or evidence |
| --- | --- |
| `1b23249` | Scoped undo targets, complete host gesture lifecycle, live snapshot/reset consumers; [editor review](../audits/EDITOR_REVIEW_2026-11-20.md). |
| `bff9a40`, `baff47a`, `c34754b` | Transactional wheel history, bounded preset reads, coherent snapshot publication. |
| `bda4f2d` | [State/cache audit and older-branch reconciliation](../audits/STATE_REVIEW_2026-11-20.md). |
| `ea63ce5` | Audio-owner reset and pitch activity/drop epoch repairs. |
| `75257bb`, `7d55df6` | Processor-owned bounded preference delegation and editor snapshot/reset review handoff. |
| `29b039a` | Runtime/pitch regressions and [runtime review](../audits/RUNTIME_REVIEW_2026-11-20.md). |
| `0299a17` | Integrated persistent snapshots, reset mailbox, host UI serialization/notification, open-editor restore, PCB persistence, zero-frame diagnostics and regression source; [integration review](../audits/INTEGRATION_REVIEW_2026-11-20.md). |
| `ffab6d2` | Qualified `juce::AudioProcessor` listener registration in the new native tests. |
| `1f21fe9`, `9d8ba78` | Queue ordering repair and its repeated-test/independent-review evidence. Original worker commits: `43d2d777670e068c6be3d6aa7724a7afca403035` and `1999eb2bf0e6d63a193b01946d2038173c2c0fbc`. |

## Remaining work and next owner

1. Read exact-head Linux/Windows CI and pluginval on the published PR. Investigate
   actual failures without weakening acceptance criteria. The combined local
   core and all six native processor/UI suites passed, and their evidence is
   archived. Check the live head for competing work before a guarded forward ref
   update; do not transfer an older CI result to a newer commit.
2. Define and implement complete runtime/control/engine/seed checkpoint capture,
   host-thread handoff, restore compatibility and migration. Current plugin
   sessions preserve parameters and UI preferences; a prepared reset seed is not
   a saved dynamic continuation. Configuration JSON already has blueprint/seed
   fields, but that does not connect full dynamic state to the plugin session.
3. Extend A/B operations to coordinated dynamic-state transfer. Current copy/swap
   changes parameters and creates one local undo record. APVTS still publishes
   individual values; it is not an atomic multi-parameter/audio-state transaction,
   and same-parameter concurrent host writes remain a coordination boundary.
4. Implement live Blueprint graph application and complete sound-preset load/save
   transactions, then fault insertion, component restoration seeds, GIF/MP4 and
   configured community/provider services. Blueprint validation and local preset
   catalog/import/export/delete now work; unavailable actions remain explicit.
   PNG export is wired.
5. Continue native host/window/transport tests, audio-thread profiling, worst-case
   reset and sustained performance measurements, detector-corpus/confidence
   qualification, and measured sidechain budgets. Keep the zero external-load
   budget until a model supplies evidence. Direct detector preparation bounds
   are integrated; detector corpus and confidence qualification remain open.
6. Reconcile older PRs item by item, retaining their branches and test contracts.
   Older snapshot acknowledgement/stopped-audio handoffs, LITE upgrade/export,
   analysis/rail telemetry and RT-trap semantics are not resolved by similar
   module names in the newer architecture.

The editor and integration reviews are scoped source reviews with regression
source; their initial checkpoints did not execute native tests. The state
worker's initial 5/5 Debug sanitized pass and runtime worker's 3/3 Release
sanitized passes qualify their own recorded sources and executions. The later
combined run and native results are recorded above and in the validation file.
No ThreadSanitizer, leak-check, real-host or production-performance qualification
is inferred from those results.

## Original source inventory and retained provenance

- Main at discovery: `44076b1f8beedeb7ec402ca1e9ca1f8ff08d56d6`.
- Candidate base: PR #15, `0c0d6897caf36d4057ac3d8db8cfb346dd5412b4`,
  branch `nodata/confident-fermat-xv16g0`, retaining its 35 commits beyond main.
- Earlier engine line: PR #10,
  `71a263718ae7ac9ed0d4a75d624929489216ef53`.
- All 15 original remote branches, exact heads, discovery-time ancestry counts
  and recovered-file source paths remain in `consolidation-state.json`.
  Already merged ancestors are not imported again. These inventory values are
  historical provenance, not assertions about today's remote heads.
- PR #10's eight graph/compiler/lifecycle/fault production files and their three
  original suites were recovered because they were absent from PR #15. Lifecycle
  snapshot validation and replacement-generation bounds were then strengthened.
- Older and newer runtime/control/media/network APIs differ materially. Remaining
  old work is preserved for semantic comparison; incompatible architectures are
  not merged wholesale.

PR #9/#10 `ScopeMath` and rail/telemetry suites use an older telemetry contract.
PR #14's `ControlHub`, `LatestSlot`, `SampleClock`, `SpscRing` and RT-trap harness
need explicit mapping to current queues and worker ownership. Its full-state
handoffs include useful generation/acknowledgement ideas, but some dynamic-state
capacities are placeholders. Earlier state/history/cache and LITE implementations
also require contract-level comparison; the state/runtime audits document those
dispositions. No PR is closed merely because the candidate has more tests.

### Historical continuation timeline

- Draft [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16) originally
  published branch `codex/cdfx-consolidation-20261119` at
  `5ccbb54a775a59fbd68b1635ff9c1a6b94e07a20`, tree
  `6582095daf5ff9d0e8f934295c6ac44aa742134a`.
- The original combined core record at
  `f7c7b6a989438b7f320a191a7b63c2ccffa2a3b9` passed 30/30 in 136.40 seconds.
  [That validation record](VALIDATION_2026-11-19.md) remains historical evidence,
  including its adverse ungated alias cases and performance limitations.
- Isolated branch `codex/cdfx-seek-epoch-20261119` started from
  `e721e9eea044b352546abd3a1b945fa1f3745038`. It added seek/pitch generations
  in `edf0c21bc8b26ca928f0df8b5ed3d0459d02275c` and stale persisted-pin
  cleanup in `63c33686bca432c807c93112e6aa65dc349d49e2`. Both were subsequently
  incorporated into PR #16 at the qualified `cb53cd8` source above.
- [Run 44662769491](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44662769491)
  used synthetic merge checkout `33fbe24c62c134998bf6fe080b592772c390300c`.
  Linux passed; Windows build/tests/packages passed, but its wrapper checked
  pluginval before the GUI process completed and retained empty reports. The
  [diagnosis](../audits/WINDOWS_PLUGINVAL_2026-11-20.md) records this old failure.
  Waiting for process completion and selecting the exact PR-head checkout
  repaired the wrapper. The later `cb53cd8` run fully passed; the diagnosis's
  pending-rerun wording describes its earlier observation only.
- Isolated branch `codex/cdfx-live-editor-bridge-20261120` began from qualified
  source `cb53cd812700e5edbcf7323653b84e470da77cfc`; its initial published
  editor source was `7c0c72df79a692f14b223a720a9da51c66d164ce`, followed by the
  `3f122df` handoff. The shared continuation now contains the subsequent repairs
  listed above. Statements at those earlier checkpoints that the live editor was
  generic, reset was unwired or UI settings were not persisted are superseded.

## Preserved engineering and acceptance boundaries

Earlier consolidation established the `cd_runtime` layer; float/double adapter
processing; oversized-block/MIDI boundaries; staged latency and aligned bypass;
worker-owned pitch analysis; checkpoint validation/rollback; finite drift, tuning
and transport handling; edition/schema/mapping checks; bounded cache/bundle
validation; and pinned CI actions without retained checkout credentials. These
remain part of the candidate rather than being discarded during the UI work.

Code audit G1, fresh code review G2, security G2 and control bug review G3 remain
scoped historical handoffs. Earlier interrupted bug/security attempts are partial
work, not completed independent audits. The new editor/runtime/state/integration
reviews do not imply a fresh whole-repository security signoff. Cache/library
operations retain their documented trusted, serialized/single-process boundary;
external filesystem races and failed pin-marker deletion are not made atomic.

All models remain synthetic. G01 hardware evidence, G02 audition, G03/G04
measured budgets, G05 real-host IPC, G06 provider operation, G07 native MP4, G08
AAX, G10 detector qualification, G11 remaining editor services and G12 complete
session continuity remain limited or open. The custom editor, reset and UI
persistence work advances G11/G12 without closing their full acceptance criteria.
ADR-0001 records the build/license decision. Do not weaken acceptance criteria or
convert source, stub, mock, sanitizer timing or documentation checks into physical
or release qualification.


## Parallel isolated preset service and PNG integrity checkpoint — 2026-11-20

Branch `codex/cdfx-preset-services-20261120-r4` was created from the exact
published PR #16 head `b3e05b652147a584ad5fb22e5715d1e61b702318`
while its [new exact-head CI run](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993)
remained queued. This branch does **not** advance PR #16 or replace that run.
The source continuation is deliberately confined to the live editor adapter,
its native UI regression and this handoff.

The live bridge now uses the existing validated, bounded `PresetLibrary`
for factory/user catalog search, canonical local import/export, and user-preset
deletion. A supplied directory constructor isolates test storage; production
uses per-user application data. Imports never overwrite an existing name;
factory presets cannot be deleted. **Load/save of the current sound remains
explicitly unavailable** until a prepared-runtime transaction can carry slot
seeds and blueprint identities alongside APVTS parameter values; do not
silently substitute parameter-only preset application.

PNG capture now fully encodes in memory and uses the existing same-directory
atomic local-file writer rather than truncating the destination before
encoding. The native UI regression extends coverage for catalog lookup,
canonical factory export/import, no-overwrite, factory deletion rejection,
user deletion and PNG decoding. These tests are **added but not yet executed**
against this new branch: the connector environment has no network-accessible
local Git clone or native JUCE dependency cache. They require exact-source
native Windows/Linux qualification before integration.

This branch has not been independently code/bug/security-reviewed: no worker
spawn facility is available in this execution context. Security follow-up
should inspect path handling, user-selected export destinations, directory
permissions/symlinks, binary atomic writes, and the boundary between disk
catalog operations and dynamic preset application. Integrate with the
coordinator's newer branch only after checking its live head and conflicts;
preserve the pending PR #16 CI attribution. No changes to the audio thread,
model calibration, releases or deployments were made.

## Isolated preset-library consolidation and scene-slot hardening — 2026-11-20

Continuation branch `codex/cdfx-preset-guard-20261120` descends from the isolated
`codex/cdfx-preset-services-20261120-r4` commit `1f1a92a5cc8d6b44a863410c2967a8d44b6132c8`,
which itself is one commit ahead of draft PR #16 source
`b3e05b652147a584ad5fb22e5715d1e61b702318`.
The original branch and PR #16 were not changed. **Do not fast-forward
the coordinator while its exact-head Windows run remains queued.**

The inherited feature connects the existing validated local `PresetLibrary`
to the production `ProcessorEditorBridge` for search, import, export and
user-preset removal, and writes PNG screenshots through atomic file replacement.
It intentionally **does not** load/save complete live sounds: APVTS-only
application cannot carry blueprint identity, prepared runtime seed or full
dynamic state. That G11/G12 boundary remains open.

A focused source bug-hunt found that `state::captureScene`,
`applyScene` and `matchesScene` indexed `Config::slots` using an unchecked
signed slot, allowing undefined behavior for `-1` or values greater than
`1`. These are non-audio state/UI helpers, so the guard rejects invalid
indices with `std::out_of_range` before any array access. Regression source
checks all three calls at `-1`, `2` and `INT_MAX`, along with unchanged
source configuration. No audio-thread exception path was added.

**Verification:** repository source mutations were published and checked for
the expected three guarded call sites. Native CMake/CTest, JUCE UI tests,
Windows/pluginval and independent code/security reviews remain to be run
on the **exact final child PR head**. The coordinator's earlier 30/30 core,
6/6 native and prior Windows pass do not qualify this descendant.
The repository is public, but this environment's container cannot resolve
`github.com` for a full local clone or fetch a native JUCE dependency cache;
the connected GitHub API supports source edits and CI inspection only.

**Next owners:** fresh independent code, bug and defensive-security reviewers
should examine (a) the new scene-slot guard and the preset UI adapter,
(b) trusted per-user directory assumptions, symlink/rename and no-overwrite
races, and (c) dynamic seed/preset load transaction design. Run
`state_preset`, `ui_snap_live_bridge`, all required native suites and
exact-head CI before consolidation. Evaluate older PR #9/#10/#14 semantics
item by item; do not blanket-merge or discard source.


## Combined isolated PR #17 and #18 candidate — 2026-11-20 UTC

This isolated continuation starts from draft [PR #18](https://github.com/djshellshoxxx/circuitdriftFX/pull/18) at `86658cf6b126e2eaabb2bac686148b76d7e7cbaa`,
which already contains the local preset library, atomic PNG output and three scene
slot guards, and imports only the separate detector production and regression
files from draft [PR #17](https://github.com/djshellshoxxx/circuitdriftFX/pull/17)
at `d12fd60d83b6c56f9941ca1dc804c5bcd14ce6b2`.
The child changes are disjoint outside handoff/state documents. Their historical
source branches and acceptance contracts are preserved, and the live PR #16
coordinator's branch is not advanced or force-updated by this candidate.

As freshly checked, the exact head `b3e05b652147a584ad5fb22e5715d1e61b702318`
of [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993),
exact head `d12fd60` of [PR #17](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44694338052),
and exact head `86658cf` of [PR #18](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44702393167)
**each passed both Linux and Windows, including strictness-5 VST3 pluginval**.
The combined branch is **not qualified by these separate passes**; run exact-head
Linux and Windows tests on the new combined commit and independently review the
result before importing it into PR #16. No local native build can run in this
connector-only environment (the checkout host cannot resolve github.com).
No fresh separate code-review, bug-hunt or security-review workers were available.
See [focused integration audit](../audits/QUALIFIED_CHILD_INTEGRATION_2026-11-20.md).

Next owner: inspect combined exact-head CI, obtain independent code/bug/security
review, and coordinate guarded integration with PR #16's current owner. Continue
full dynamic engine/control/seed session checkpoints, coordinated A/B handoff
and semantic reconciliation of #9/#10/#14 separately. Models are synthetic;
no physical, release or deployment qualification is implied.

## Isolated atomic no-overwrite preset creation follow-up (2026-11-20)

Child continuation from draft PR #19 source `e9c00c51cdf155127cd839c446126ae1d8f9f6c6` on
`codex/cdfx-preset-atomic-create-20261120`. A source audit found that the existing local preset library's
`save(name, ..., overwrite=false)` checked existence and then used
`saveStateFileAtomic()`, whose final rename/MoveFileEx replaced the destination.
Two independent message-thread writers could therefore overwrite one another,
violating the no-overwrite import/save promise. The repair adds
`saveStateFileAtomicNoReplace()` using an exclusively installed destination:
POSIX hard-link creation after an fsynced temporary file, or Windows MoveFileEx
without REPLACE_EXISTING. The old overwrite writer remains unchanged in behavior.
PresetLibrary selects the correct mode; regression source covers pre-existing
content and two competing writers, including winner-content and temp cleanup.

This is **isolated reviewable work**, not a claim of compiled or verified native
tests. Run exact-head Linux state_preset and Windows/pluginval CI; review
filesystem symlink races, Windows locks/path encoding, destination directory trust,
and the Windows filesystem's no-replace semantics. This does not complete
full runtime/seed/session persistence or the live sound-load transaction.
Separate independent reviewers are not available in this connector execution.
Do not promote over PR #19 while its exact-head Windows validation is active.

## G12 child: fail-closed checkpoint compatibility — 2026-11-20

Isolated branch `codex/cdfx-session-checkpoint-guard-20261120` starts from exact PR #20 source `8deb187da5cb8e0428b61d50b4a19be7307e6385`.
PR #16/#19/#20 and main remain unchanged. The `decideRestore` gate
checked electrical and thermal sizes but not the RNG count, so it could
incorrectly authorize exact stochastic continuation with an incompatible
prepared RNG layout. Programmatic in-memory checkpoints also bypass the JSON
parser's finite and sample-index admission checks.

The child adds explicit `RuntimeIdentity::rngSize` (zero is *not* a wildcard),
requires all three prepared vector sizes to match, and rejects invalid
in-memory dynamic scalars or schema-v1 sample-index overflow. An incompatible
checkpoint chooses `ResetWarm`, never partial restoration. `test_state_safety`
now covers success, RNG mismatch, nonfinite dynamic state, sample-index
overflow and absent checkpoint. The review note is
[SESSION_RESTORE_GUARD_2026-11-20.md](../audits/SESSION_RESTORE_GUARD_2026-11-20.md).

Source-replacement assertions and JSON preflight passed. Full compilation,
exact-head CI, pluginval and independent code/bug/security review **have not
yet run** for this child. PR #20 Linux passed and Windows was still building
at inspection, but neither qualifies this new commit.

This is an incremental G12 restore guard, **not** complete runtime/seed/session
persistence. Engine/control capture, prepared seed identification, audio-owner
snapshot publication, host persistence and stopped-audio restore transactions
remain unimplemented. Fresh review workers were unavailable in this runtime.
Next owner: check exact-head Linux state_safety/Windows pluginval, review caller
RNG size and state integrity, then integrate only after a guarded live-ref check.

## Exact-head restore CI repair and forged-vector admission — 2026-11-20

[PR #21 run #44731856158](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44731856158) at `0487a63d07e58a228a32705f03272efacc8fdba9` **failed Linux CTest (29/30)**: `acceptance_core` at `tests/test_main.cpp:515` expected Restore despite an omitted prepared `rngSize`. The new guard correctly rejected two synthetic checkpoint RNG words against the default expected zero. `state_safety` and the other 28 tests passed; Windows was skipped. Parent PR #20 completed both platforms including pluginval.

An isolated child corrects the synthetic prepared identity to declare its two RNG words and asserts incompatible RNG layouts fail closed. Production `decideRestore` now also rejects matching-but-oversized *programmatically constructed* electrical, thermal and RNG vectors (4096/4096/64) before installing an exact checkpoint. These are the existing parser limits, not relaxed acceptance criteria. Three new `state_safety` regressions exercise oversized identities and clean restoration. See [focused audit](../audits/SESSION_RESTORE_CI_FOLLOWUP_2026-11-20.md).

This child requires its **own exact-head Linux/Windows/pluginval result**. Native testing is unavailable locally because GitHub DNS is inaccessible from the container. Independent reviewers cannot be spawned through this connector; code, bug-hunt and defensive-security review remain pending. Keep PR #16/#19/#20/#21 and `main` intact. Full runtime/seed/session capture, state migration and stopped-audio restoration are still open G12 work.

## Session writer checkpoint preflight — 2026-11-20

Draft PR #22 source `7a95379554aba5d3d42f441436e58cae43ddec4b` is now **qualified on its own exact head** in [CI #44742353994](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44742353994): Linux sanitized CTest and Windows VST3 pluginval strictness 5 passed. The parent #21 failure is superseded **only for its corrected descendant**.

The new isolated continuation `codex/cdfx-session-writer-preflight-20261120-r11` closes a separate G12 serialization-admission gap: `saveState` now preflights *programmatically built* session checkpoints against schema-v1 identity, finite scalar, vector-size and finite-state bounds before JSON construction. A scene cannot carry a checkpoint in local `saveState`, while `publishScene` continues intentionally stripping private state. A mismatched but well-formed checkpoint is still serializable, with actual restore compatibility decided independently.

Regression source in `tests/test_state_safety.cpp` covers oversized electrical/thermal/RNG, nonfinite state/rate, invalid oversampling, empty model revision, scene-only restriction and a valid session roundtrip. See [focused audit](../audits/SESSION_WRITER_PREFLIGHT_2026-11-20.md). **Child exact-head CI is still required.** No native tests ran through the connector, and no separate review workers were available. This is a partial checkpoint persistence safety improvement, *not* full engine/control/seed capture or real host dynamic continuity. Next owner: qualify the child, review parser/writer symmetry independently, and advance only with a guarded coordinator check.

## G12 whole-document session writer symmetry — 2026-11-20

Child continuation `codex/cdfx-writer-roundtrip-20261120-r12` forks the exact qualified [PR #23](https://github.com/djshellshoxxx/circuitdriftFX/pull/23) head `b9994290a8db03b50535f597ab76649e4b347b2f`. [CI #44749975047](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44749975047) passed Linux and Windows/pluginval for **that parent only**. Parent heads, main and historical branches are untouched.

The previous checkpoint-only writer preflight left invalid directly assembled envelopes serializable and silently normalized unsupported container types; reserved metadata could also shadow schema fields. The child adds fail-closed parameter/metadata uniqueness, container checks and a production-reader roundtrip validation of the complete serialized schema-v1 document. It adds targeted `state_safety` source regressions; see [audit](../audits/SESSION_WRITER_ROUNDTRIP_2026-11-20.md). Existing known-valid fixture output must remain byte-identical. The serializer stays exclusively off the audio thread.

**Not yet qualified:** child Linux state_safety/sanitizers, Windows/pluginval, independent review, and full G12 engine/control/seed capture. The current tool environment cannot spawn independent reviewer workers or clone GitHub for native compilation. Next owner: inspect exact-child-head CI, review recursive bounds/failure atomicity, then coordinate guarded integration with PR #16's live owner. Complete full stopped-audio host-state persistence separately; do not equate writer integrity with working session continuity. No releases/deployments or changes to main.

## G12 lossless scene/session blueprint cardinality — 2026-11-20

An isolated child of draft PR #24 adds strict `saveState` cardinality checks before `toJson`, because a directly constructed `Scene` with two blueprints was otherwise silently serialized with only the first. Invalid document-kind enum values are rejected rather than normalized into scenes. Valid two-blueprint sessions still roundtrip. Targeted `state_safety` regressions and [a focused audit](../audits/SCENE_BLUEPRINT_CARDINALITY_2026-11-20.md) document the exact behavior and reviewer handoff.

PR #24's [run #44759057463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44759057463) had passed Linux but Windows/pluginval was still in progress at selection. The new child needs its **own exact-head Linux/Windows/pluginval** and independent code/bug/security review. Container GitHub DNS prevents native build; separate reviewer agents are not exposed. Full engine/control/seed capture, stopped-audio restore, dynamic A/B and older PR #9/#10/#14 reconciliation remain open. Leave coordinator PR #16, all source branches and `main` unchanged.
