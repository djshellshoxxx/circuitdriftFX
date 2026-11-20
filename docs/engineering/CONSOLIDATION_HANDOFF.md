# CircuitDriftFX consolidation handoff — 2026-11-20

## Current checkpoint — rounds 14–18

Draft [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16), branch
`codex/cdfx-consolidation-20261119`, is the integration and CI authority.
Read its live head before a guarded forward update. Original source branches
and `main` at `44076b1f8beedeb7ec402ca1e9ca1f8ff08d56d6` are preserved.

Current production/review source is local
`4dcd584f995c261b189774ea8f416fda40ad97a0`. The latest test-strengthening
source is local `8ff0adfea571e977adec128622209f63bf997d1d`, remote Git object
`65b3a259c5dc657e59adea3a2df8a2efbb82e155`, tree
`476353329e03b5b9657af170cf735ac9ddc00163`. The containing publication adds
evidence and handoff documentation. Local verification passed **31/31 sanitized
core tests and 6/6 native applications**; new exact-publication CI is required.

[The continuation commit map](../evidence/commit-map-continuation-r14-2026-11-20.json)
records full-tree equality for every new source commit. The parent histories
include both the reviewed local continuation and the concurrent published
`02a7331cc687e710f3a3c7c0ab023e651601685e`, joined in local merge
`9630ae03b4be4bc74b1d01d6bbf42b5259d8faec`. The intermediate merge retained
a coherent local source baseline; subsequent commits reconciled the unique
contracts and tests from the parallel implementation.

## Consolidated sources and ownership

PR #18–#25 are included by ancestry through
`c6ebb22daab9c46edffa761b21f3fd45320e5074`. PR #17's three detector
code/test files and audit were copied into #19 and are byte-identical in that
stack. Evolving handoff/state records remain historical provenance. PR #17–#25
are closed after exact live-head and source-inclusion verification. All nine
original branch heads and `main` were verified retained.
Older PR #9/#10/#14 still need item-level semantic reconciliation.

The [historical handoff](CONSOLIDATION_HANDOFF_HISTORY_2026-11-20_R14.md)
preserves earlier editor work, child branches, original inventory and the
concurrent R14 checkpoint. Its old instructions to hold coordinator refs or
await now-completed CI are historical dispositions. This file, the live refs,
`CLAUDE.md`, ADR-0001 and the current machine-readable state govern continuation.

## Implemented behavior

### Blueprint validation preview

One portable `ValidationWorker` runs the real compiler with one active request
and one replaceable pending request. Explicit cancellation stops active work,
advances generation and clears pending/completed results; publication requires
both a current generation and an unstopped token. Compiled candidates are
destroyed on the worker. The duplicate plugin-specific worker is not retained.

The Full edition supports editing or pasting a draft and inserting the synthetic
**Example RC** fixture. Oversized and LITE requests are rejected before UTF-8
conversion or a worker copy; the worker independently enforces the 1 MiB limit.
Without active audio, validation uses an explicitly labeled preview configuration.
With audio, reports identify the active sample rate and oversampling.

Reports show component/node/unknown counts, diagnostic check/code and component
pin, arithmetic and storage estimates, configuration and synthetic provenance.
They do not present estimated arithmetic as measured CPU time. Draft changes,
audio-configuration changes, session replacement, reload and relevant slot
changes invalidate obsolete results; both pending and already-delivered reports
are covered. Panel destruction cancels pending work.

Live graph Apply remains unavailable. Before enabling it, implement graph
installation, crossfade, retirement and undo ownership, and recheck captured
Fs/OS/session epoch at delayed approval/dispatch as well as text, slot and
capability. The current live adapter does not expose that unfinished transaction.

### Presets and local files

Factory/user browsing, import/export and user deletion are connected. Initial
and manual discovery run on one lazy background worker. Search filters accepted
cached rows. Refresh and successful import/delete coalesce requests and reject
obsolete results; the panel exposes loading, errors and truncation.

Scans visit at most 5,000 entries and admit at most 32 MiB of reads, including
failed/partial reads. Query-only changes preserve an explicitly selected ID when
it remains visible, even if its row moves. A newly accepted catalog generation
or manual Refresh clears selection. The generation is read after listPresets,
which may itself accept a newly completed scan. Double-click uses the callback's
explicit preset ID.

Complete sound-preset Load/Save remains unavailable until parameters, blueprint,
seed and runtime state can move through a coordinated transaction. Existing
atomic no-overwrite installation and PNG output are retained. POSIX temporary-name
cleanup after a successful no-replace installation is explicitly best effort.

### State integrity and editor correctness

State admission counts the complete encoded envelope before recursive copies:
field names, escaping, separators, arrays, opaque payloads and typed checkpoint
contents. It retains per-blueprint/metadata sublimits, root/session depth
accounting, inactive-storage/type rejection, bounded UTF-8 checkpoint identities
and parameter-count/duplicate guards. Resource admission precedes publication
semantic traversal. Private UI/checkpoint/metadata fields intentionally stripped
from a published scene are excluded from publication admission. The final reader
roundtrip and output-on-success contract remain.

Restore requires a session-kind document and valid checkpoint identities before
matching runtime configuration. These repairs do not implement full plugin
dynamic-session capture or migration.

Plain editor text and placeholders track theme changes. A separate safe listener
also refreshes stored glyph colors restored by undo/redo, preserving document,
editing history, caret, selection and existing callbacks. Registrations detach
on destruction and reparenting.

Earlier live custom-editor integration, parameter gestures/history, persistent
telemetry, audio-boundary reset, host-persisted UI preferences, pitch epochs and
parameter-only A/B remain implemented; their original evidence is preserved in
the historical handoff. A/B is not yet a coordinated dynamic-state transaction.

## Verification and audit handoff

| Exact source and scope | Recorded result |
| --- | --- |
| Prior coordinator `b3e05b6` | Linux/Windows/pluginval passed in [run 44690495993](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993). |
| Incoming child stack `c6ebb22` | Linux/Windows/pluginval passed in [run 44764678463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44764678463). |
| Earlier local `1bfc6f8` | 31/31 sanitized core, 333.79 s, plus 2/2 native, 20.11 s. |
| Earlier combined `2bad426` | 2/2 native, 12.97 s; includes asynchronous catalog and delivered-report freshness. |
| Theme regression, unchanged production `2bad426` | New test source from `125b217` failed nine assertions in 11.03 s before repair. |
| Final merged core `4dcd584` | **31/31 passed, 211.91 s**, GCC 14.2 ASan/UBSan/float-cast-overflow. Production and most tests O1; the large state_safety test translation unit O0 after the recorded resource limit. |
| Final merged native `8ff0adf` | **6/6 passed, 54.20 s**, Clang 19.1.7 Custom O0, no sanitizers: processor parameters, contract, live bridge, panels, shell and workbench. |
| Containing publication | Requires its own exact-head Linux/Windows/pluginval. Earlier results qualify only their source. |

The only later source change after the merged core run is the native preset
regression header. Portable source/build inputs are unchanged. Documents are
validated separately after the final update.

Five successive code-audit, bug-hunt and defensive-review sets completed.
The [rounds 14–18 audit](../audits/CONTINUATION_ROUNDS_14_18_2026-11-20.md)
records findings, fixes, exact pins and each worker's executed versus source-only
scope. The final three reviewers found no new actionable production defect.
Their test-proof gap was addressed by publishing a new snapshot inside the test
bridge's listPresets call; the delayed Apply requirement remains an explicit gate.

Evidence:
[round 14](../evidence/continuation-round14-2026-11-20.json),
[rounds 15–16](../evidence/continuation-round15-16-2026-11-20.json), and
[merged rounds 17–18](../evidence/continuation-round17-18-2026-11-20.json).
Offscreen native/source review evidence does not establish real-DAW, optimized
performance, TSan, leak, OS-I/O deadline or physical-model qualification. Local
leak detection was disabled under the tracer. Models remain synthetic and
filesystem operations retain the documented trusted-directory assumption.

## Next continuation

1. Read the live PR #16 head and exact-head CI; repair concrete failures without
   relaxing assertions or acceptance gates. Use an expected old SHA and forward
   ref update, preserving concurrent work and original branches.
2. Resume the existing GIF work branch
   `codex/cdfx-gif-capture-20261120-r14` before creating another implementation.
   Its observed head was `ecae510b72c0ef4327bacfb6e4aa35d08eea2748`; reread the
   live head and owner evidence. Three R19 reviewers completed the
   [GIF preintegration audit](../audits/GIF_PREINTEGRATION_REVIEW_2026-11-20_R19.md).
   Fresh R20 backend, UI and resource workers are repairing final-frame drain,
   independent deadline/20 Hz timing, owned visual surfaces and declared sizes,
   cumulative allocation bounds, zero-copy output and allocation-safe failure
   handling in isolated worktrees from 8ff0adf. Resume their work and regression
   handoff before creating another implementation. GIF is not yet integrated
   or qualified by this checkpoint; its original branch remains preserved.
3. Complete runtime/control/engine/seed checkpoint capture, host-thread handoff,
   stopped-audio restore, compatibility/migration and coordinated dynamic A/B.
   Sound-preset Load/Save and complete scene transactions depend on that contract.
4. Complete graph Apply, fault insertion/restoration and remaining media/provider
   services under their separate contracts. MP4 requires a real codec capability.
5. Reconcile older PR #9/#10/#14 semantics and continue measured native-host,
   detector-corpus, reset/CPU, sustained-performance and physical-reference gates.

The existing hourly continuation remains enabled. Each successor should read
the current handoff and use fresh code, bug and defensive reviewers for changed
work, preserving exact source and evidence. Repeat verification to resolve a
concrete risk or required gate. Credit balance is not observable. This remains
development work with no release, deployment or paid-service provisioning.
