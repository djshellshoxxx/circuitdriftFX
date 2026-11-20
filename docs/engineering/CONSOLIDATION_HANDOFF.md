# CircuitDriftFX consolidation handoff — 2026-11-19

## Goal and current authority

The owner requested consolidation of genuinely unique work, continuation of incomplete
features, separate code/bug/security workers, and fresh-worker handoffs. Source
integration and verified repairs are authorized. Preserve existing branches and
acceptance criteria; do not infer physical calibration, release qualification or
permission to provision paid services from this development request.

Read this file, the current live refs/PRs, `consolidation-state.json`, `CLAUDE.md`,
the build ADR and the audit handoffs before selecting work. Repository state can
advance independently of a saved prompt. Avoid overlapping another active worker.

## Source reconciliation

- Main at discovery: `44076b1f8beedeb7ec402ca1e9ca1f8ff08d56d6`.
- Candidate base: PR #15, `0c0d6897caf36d4057ac3d8db8cfb346dd5412b4`
  (`nodata/confident-fermat-xv16g0`), retaining its 35 commits beyond main.
- Earlier substantial engine line: PR #10,
  `71a263718ae7ac9ed0d4a75d624929489216ef53`.
- All 15 original remote branches and their exact heads are recorded in
  `consolidation-state.json`. Already merged ancestors are not imported again.
- PR #10's blueprint graph/compiler, lifecycle and fault modules were genuinely
  absent from PR #15. Eight production files and their three original test suites
  were recovered; their source provenance is recorded in the inventory. Lifecycle
  snapshot validation was then strengthened in a separate commit.
- The older and newer runtime, control, media and network APIs differ materially.
  Remaining old work is retained for semantic comparison, not discarded or
  declared equivalent because a newer similarly named module exists.
- PR #14's control handoff/snapshot/trap implementation, remaining PR #9/#10
  runtime/analysis/edition work, and prior documentation PRs still need item-level
  reconciliation. Do not close them merely because this candidate contains a
  larger engine or has more tests.

## Engineering completed in this candidate

- Added `cd_runtime` between control/engine and JUCE. It sanitizes host parameters,
  maps the two LITE controls, processes all samples of oversized blocks, preserves
  MIDI event boundaries, handles zero-frame calls, and reports staged latency
  changes. Audio buffers are prepared outside callbacks.
- Connected both float and double plugin callbacks to the actual circuit runtime,
  including declared latency, aligned bypass, transport and bounded MIDI input.
  Pitch analysis runs on a separate worker joined before preparation/release.
- Repaired UI gesture identity/lifetime, numeric input validation, a weak-pointer
  settings callback, and the narrow Faults selector caption.
- Repaired snapshot stress scheduling without weakening coherence assertions.
- Hardened cache writes/pins and bundle parse/serialize/replay/co-render validation.
  Detailed scope and limitations are in the generation-2 security handoff.
- Recovered blueprint primitives, rejected inconsistent lifecycle snapshots, and
  prevented replacement-generation wrap.
- Validated nested engine/DSP checkpoint indices before conversion and mutation,
  with an allocated rollback image and continuation regressions.
- Completed portable drift clock/snapshot validation, deterministic reset, finite
  tuning/transport handling, and clamp-before-convert control parameters.
- Fresh code review repaired held MIDI notes on event overflow, prepared-mode
  changes affecting active processing, and numeric editor fields changing meaning.
- Added edition/schema/mapping markers and validation before parameter-state
  replacement. Known-ID legacy configuration stays readable in its own edition.
- Completed a sample-zero MIDI flush path without clock advancement, including
  bounded overflow release and no pitch-worker reset.
- Pinned official CI action sources and disabled retained checkout credentials.

## Verification status at this checkpoint

**The final combined core passed 30/30 suites** with GCC 14.2 and
ASan/UBSan/float-cast-overflow in 136.40 seconds. Read the
[consolidated validation record](VALIDATION_2026-11-19.md) for the qualified source
commit, complete log, commands and exact limitations. Generated registry/plugin
checks and documentation validation also passed.

Final processor and plugin consistency sources passed syntax compilation against
the pinned JUCE source; the fresh reviewer similarly checked affected UI files.
Full linking/execution remains a Windows CI gate. The PR's initial CI run was
queued at publication. No native, leak-check, physical calibration or production
CPU qualification is inferred from this local result. Adverse ungated alias cases
remain visible in the full CTest output.

## Worker handoffs

Code audit G1, fresh code review G2, the security G2 successor, and control bug
review G3 produced commits and handoffs. Earlier bug/security worker attempts
were interrupted by automatic content screening; their partial files were retained,
and the coordinator completed bounded engine/cache/blueprint maintenance. They
must not be described as completed independent audits. Additional fresh-thread
creation and follow-up activation reached this session's worker thread limit.
Future runs must start from this durable checkpoint and recheck live ownership.
An hourly continuation task is enabled, starting after this development session;
its future runs resume the live PR and this handoff. Credit balance is not exposed.

## Published checkpoint

Draft [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16) targets main.
Its branch is `codex/cdfx-consolidation-20261119`. The initial remote commit is
`5ccbb54a775a59fbd68b1635ff9c1a6b94e07a20`, with tree
`6582095daf5ff9d0e8f934295c6ac44aa742134a`, verified identical to the local
consolidation tree. Further changes use guarded forward ref updates. Read live
head/checks before continuing; source and tests may advance after this record.
The existing main and original development branches are retained.

## Fresh-worker queue

1. Read Windows CI on the exact published commit and resolve actual failures
   without reducing acceptance criteria. Refresh affected local tests for new changes;
   the recorded combined core run already passed.
2. Independently review the new cache accounting/typed-blueprint and engine checkpoint
   repairs, including the separate generated-port-source budget in the newer solver.
3. Implement the real custom `EditorBridge` with actual transactions and supported
   services. The live editor remains generic; snapshots use `FakeBridge`.
4. Complete dynamic session checkpoints, state migration policy, staged quality
   diagnostics, and measured sidechain budget integration. A zero external load
   budget is retained until a model supplies its value; no calibration is guessed.
5. Resolve seek/reset coordination between the callback and pitch worker without
   resetting shared control state concurrently from the audio thread.
6. Reconcile older analysis/edition/RT-trap and runtime work module by module.
   Then address remaining community/native media/host IPC and performance gates.

All models remain synthetic. G01 hardware evidence, G02 audition, G03/G04 measured
budgets, G05 real-host IPC, G06 provider operation, G07 native MP4, G08 AAX,
G10 detector qualification, G11 custom editor/backend and G12 complete session
continuity remain limited or open. The build/license decision has evidence in
ADR-0001; do not repeat the old claim that no dependency/license decision exists.

No release or deployment is part of this checkpoint.

## 2026-11-19 hourly follow-up: isolated seek/pitch generation fix

A separate, non-overlapping child branch `codex/cdfx-seek-epoch-20261119` was
created from PR #16 exact head `e721e9eea044b352546abd3a1b945fa1f3745038`
to preserve its running Windows qualification. Engineering commit
`edf0c21bc8b26ca928f0df8b5ed3d0459d02275c` adds a bounded, atomic
producer-side pitch discontinuity marker, a worker-side queued-sample flush,
generation-tagged publications and runtime epoch detection. Its new detector
and runtime tests cover immediate stale-result invalidation, queued pre-seek
discard/reacquisition, zero-frame epoch changes and callback allocation limits.

**This child commit has not yet passed CI or been incorporated into PR #16.**
At inspection the exact PR #16 head passed Linux and its Windows job was still
building the plugin; do not cancel that run with a ref move. Once that exact-head
Windows run finishes, investigate failures using its job log (or record timeout),
review the child patch independently, run affected tests, and only then
fast-forward the PR continuation ref with a checked expected SHA. Any new PR CI
run qualifies only its own SHA. Native plugin/host validation and independent
code/bug/security reviewer signoffs remain open. No worker spawn facility was
available in this automation environment, so this entry does not claim those
reviews.

Pitch-epoch limitation: hosts that do not supply sample-position discontinuities
cannot be inferred to have sought; the adapter may need explicit host transport
callbacks. If a seek races a worker already consuming a chunk, generation checks
hide its old publications, but some just-after-seek samples may be dropped until
the worker observes the new generation. Reacquisition is from subsequently
accepted post-seek samples; no audio-thread filter reset is performed.

The earlier PR #9/#10/#14 item-level reconciliation and custom editor transaction
layer remain separate. Do not treat this follow-up as source integration of those
APIs. Preserve original development branches and core acceptance criteria.

## Cache integrity follow-up on isolated branch

Commit `63c33686bca432c807c93112e6aa65dc349d49e2` corrects a
confirmed persisted-cache bookkeeping defect: successful cleanup of a corrupt
entry or discovery of a missing on-disk entry previously left its `.pin`
marker behind. A subsequent write using the same key could inherit the stale
pin after reopening the cache, contrary to the deletion intent. The implementation
now removes the associated marker when forgetting that entry and adds a
regression that corrupts a previously pinned entry, checks marker cleanup,
restores the key and reopens it. Filesystem deletion errors can still leave
orphan markers (and external concurrent modification is unsupported); do not
claim complete multi-process atomicity. The change has not yet run under CI.

Child-branch last production commit: `63c33686bca432c807c93112e6aa65dc349d49e2`.
Do not count the base PR #16 Linux pass as verification of this later code.
