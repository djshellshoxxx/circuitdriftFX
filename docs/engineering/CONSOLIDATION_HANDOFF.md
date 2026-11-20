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
- Pinned official CI action sources and disabled retained checkout credentials.

## Verification status at this checkpoint

Validation is ongoing. Do not copy an older green result as qualification of a new
tree. Native core compilation uses GCC 14.2; local ASan/UBSan/float-cast-overflow
checks require `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer cannot operate
under this environment's ptrace. CI retains its normal sanitizer configuration.

Completed targeted evidence, with exact commands in `docs/audits/`:

- The pre-final-integration core baseline passed 23/23 suites. This is historical
  evidence, not a claim that the final changed tree has already passed.
- Engine runtime/checkpoint maintenance passed 2/2 suites; FIR/cascade checkpoint
  rejection passed the oversampling suite. Final rollback ordering needs rerun.
- Control maintenance passed all seven affected suites, the additional binary64
  sample-rate boundary, then four suites for finite tuning/transport handling.
- Fresh runtime review passed the ASan/UBSan processor suite with callback
  allocation checks. Numeric input regression also passed with sanitizers.
- Earlier cache and bundle repair suites passed 2/2. The coordinator is completing
  the subsequent cache accounting/typed-blueprint compatibility pass.

Full JUCE linking exceeded local memory. The fresh code reviewer did successfully
syntax-check the processor, affected UI files and relevant tests against the exact
pinned JUCE source. Successful syntax checks are not host or plugin execution.
Windows CI must verify parameters, UI snapshots, format builds and pluginval.
The source PR #15 failures were `state_queue` scheduling and `ui_snap_panels`
caption clipping; both repairs remain in the candidate without weaker assertions.

## Worker handoffs

Code audit G1, fresh code review G2, the security G2 successor, and control bug
review G3 produced commits and handoffs. Earlier bug/security worker attempts
were interrupted by automatic content screening; their partial files were retained,
and the coordinator completed bounded engine/cache/blueprint maintenance. They
must not be described as completed independent audits. Additional fresh-thread
creation and follow-up activation reached this session's worker thread limit.
Future runs must start from this durable checkpoint and recheck live ownership.

## Fresh-worker queue

1. Finish final integrated sanitizer/CMake checks and read Windows CI on the exact
   published commit. Resolve actual failures without reducing acceptance criteria.
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
