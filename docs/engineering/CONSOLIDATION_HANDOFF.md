# CircuitDriftFX consolidation handoff — 2026-11-20

## Current checkpoint — control repairs and bounded snapshots

Draft [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16), branch
`codex/cdfx-consolidation-20261119`, is the integration and CI authority.
Read its live head before a guarded forward update. Original source branches
and `main` at `44076b1f8beedeb7ec402ca1e9ca1f8ff08d56d6` are preserved.

Current local source is `3fb0846ff0673ea70bb94c404e6f68ce7045cc28`, corresponding
remote Git object `964eb787e538fc1260098d01e096e3ca5fb51d17`, tree
`460f96bdf87dbfa3f063969027c75f127246e8a4`. The published branch still points to
`6f26b38827b849e9f89c15613f63726036e8c26b` while aggregate qualification is active.
The [control source map](../evidence/commit-map-control-checkpoints-2026-11-20.json)
records exact complete tree equality; creating source objects has not moved the branch.

R30 repaired ignored-MIDI processing boundaries, preserved scale/root glides,
selected notes in the effective cutoff domain, and preserved/bounded sidechain
history. Its test-only baseline reproduced 129 assertions across three suites;
the repair passed **11/11 sanitized suites in 129.24 s** at 00e2957. R31 found
the changed engine algorithm still accepted the old checkpoint identity. The
two-assertion baseline now passes with revision `C0DE0002`, rejecting without mutation.

Fixed-size [control snapshots](CONTROL_SNAPSHOTS.md) now compose six components
for Manual/MIDI with no harmonic-map lifetime. All R32 reviewers completed;
their shared drift-rate finding reproduced 28 assertions before repair. The
fixed component suite and nine affected suites pass **10/10 in 21.15 s** at
3fb0846. The aggregate compiles; fresh R33 code/security review and aggregate
behavioral test authoring are active. Six native plugin/editor suites pass
**6/6 in 57.40 s** at the same source 3fb0846; aggregate behavior remains a
separate pending gate.
Root owns all compiler/test execution and integration. Read the
[R29–R33 audit](../audits/CONTROL_CHECKPOINTS_2026-11-20_R29_R33.md) and
[execution evidence](../evidence/control-checkpoints-2026-11-20-r30-r33.json).

The preceding published source `1a3f8f35efd5080f60caf18172ec4454c26dea19`, remote
`452bd97b36dd1bb0227a633ed2a42b558261a9e8`, tree
`7af2c5b76ccf7badead4c314261b6eb7cf7d9f67`, includes GIF/build-gate work and
coherent host restoration with the stopped-audio exception fallback. Its native
set passed **6/6 in 59.50 s**. Both failure baselines reproduced four assertions.
All R28 reviewers completed without a new actionable finding in assigned scopes.

The [host-restore source map](../evidence/commit-map-host-restore-2026-11-20.json)
records verified local/remote tree equality through this source. The containing
publication adds evidence and handoff documents; read the live PR for its SHA.
Earlier GIF scope has separate 3/3 sanitized resource/state, 1/1 strict-decoder,
6/6 native and 63-frame independent decoding evidence. Build source 51029e0 has
passing actual native configuration and 24-case dependency fixtures. These
results retain their original pins; exact-publication Linux/Windows/pluginval
and execution of the Windows 16-case validator harness remain required.

[The GIF source map](../evidence/commit-map-gif-2026-11-20.json) records complete
local/remote tree equality, retaining the
[earlier continuation map](../evidence/commit-map-continuation-r14-2026-11-20.json).
The lineage retains both concurrent continuation histories through merge 9630ae0
and the original GIF branch ecae510 through merge 5e7057e. Reviewed implementation
and regression fixes follow those merge points. The original GIF branch was
reread at ecae510 and remains intact.

The preceding publication 338e952 has separate 31/31 sanitized core and 6/6 native
baseline evidence. Those results are not relabeled as a new full-core GIF run.
All R25–R32 worker sets completed before their successors began. R26 build-gate
and R28 host-fallback reviews found no new actionable issue in assigned scopes.
Current R33 ownership and remaining snapshot boundaries are described above.

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

### Silent GIF capture

The session owns a bounded 1–15 s, 20 Hz monotonic timeline and immutable
request. It expires independently of panel refresh. Missed frame opportunities
repeat encoded images, preserving 50 ms delays; manual stop rounds only its final
partial slot and drains already-admitted frames. Restart rejects old frame tokens.
The editor renders only its selected Easy/Workbench subtree into declared-size
software images, including while the drawer covers it. Hidden editors stop
painting; destruction safely finalizes the admitted sequence.

GIF canvases are 540×960, 540×540 and 960×540. PNG canvases are 1080×1920,
1080×1080 and 1920×1080. Changing format requires destination reselection; the
chosen path is never rewritten after confirmation. PNG/start errors remain
visible, and a separate warning reports repeated frames above 2% without hiding
the result. Encoder output is capped at 32 MiB. Declared capture buffers/workspaces
total at most 60,654,080 bytes below a 64 MiB admission limit; this is not whole-process
RSS. Atomic installation publishes Completed before best-effort diagnostic allocation.

See the [GIF repair/review record](../audits/GIF_INTEGRATION_2026-11-20.md) for
ordering, lifetime, memory and destination contracts, worker handoffs and exact
execution. MP4, audio-synchronized output and full G11 qualification remain open.

## Verification and audit handoff

| Exact source and scope | Recorded result |
| --- | --- |
| Prior coordinator `b3e05b6` | Linux/Windows/pluginval passed in [run 44690495993](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993). |
| Incoming child stack `c6ebb22` | Linux/Windows/pluginval passed in [run 44764678463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44764678463). |
| Earlier local `1bfc6f8` | 31/31 sanitized core, 333.79 s, plus 2/2 native, 20.11 s. |
| Earlier combined `2bad426` | 2/2 native, 12.97 s; includes asynchronous catalog and delivered-report freshness. |
| Theme regression, unchanged production `2bad426` | New test source from `125b217` failed nine assertions in 11.03 s before repair. |
| Pre-GIF merged core `4dcd584` | **31/31 passed, 211.91 s**, GCC 14.2 ASan/UBSan/float-cast-overflow. Production and most tests O1; the large state_safety test translation unit O0 after the recorded resource limit. |
| Pre-GIF merged native `8ff0adf` | **6/6 passed, 54.20 s**, Clang 19.1.7 Custom O0, no sanitizers: processor parameters, contract, live bridge, panels, shell and workbench. |
| GIF resources `eb97e54` | **3/3 passed, 6.03 s**, GCC 14.2 ASan/UBSan/float-cast-overflow: media, state_safety, state_preset. |
| Strict decoder `aedef7b` | **1/1 passed, 0.57 s**, same sanitized media suite with independent EOI fixtures. |
| Final GIF/native `b84d2ba` | **6/6 passed, 61.44 s**, Clang 19.1.7 Custom O0, including production editor/session regression hooks. |
| Independent GIF outputs from `b84d2ba` | Pillow 13.0.0 fully decoded **63 frames across 4 fresh files**; dimensions, 50 ms delays, duration, loop metadata and detailed pixels matched expectations. |
| First host repair `90d647d` | **6/6 passed, 58.89 s**; fresh review subsequently found the stopped-audio exception composition edge. |
| Three-state test `9240074` | **Four expected failures, 4.36 s**, unchanged production; Full/LITE audio retained A while saves retained B after failing C. |
| Final host fallback `1a3f8f3` | **6/6 passed, 59.50 s**; all R28 code/bug/defensive reviewers completed without new actionable findings in scope. |
| Containing publication | Requires its own exact-head Linux/Windows/pluginval. Earlier results qualify only their source. |

The scoped GIF runs qualify their recorded source. Portable production inputs
remain unchanged from eb97e54 through b84d2ba; aedef7b strengthens the shared
native/media decoder and has its own passing sanitizer run. The final native
suite consumes that stricter decoder. Documents are validated separately.

Fresh workers completed each round before its successors began. The
[rounds 14–18 audit](../audits/CONTINUATION_ROUNDS_14_18_2026-11-20.md) retains
prior consolidation findings. [Rounds 19–24](../audits/GIF_INTEGRATION_2026-11-20.md)
record the original GIF review, backend/UI/resource repairs, strict decoder,
registry/destination/warning fixes, three clean R23 source reviews, first-error
layout repair and independent R24 decoding. R24 also found two build-gate defects;
R25 owners repaired them and fresh R26 code/defensive review found no new
actionable defect. Actual Windows harness/pluginval execution remains required.

Evidence:
[round 14](../evidence/continuation-round14-2026-11-20.json),
[rounds 15–16](../evidence/continuation-round15-16-2026-11-20.json), and
[merged rounds 17–18](../evidence/continuation-round17-18-2026-11-20.json),
[GIF execution](../evidence/gif-integration-2026-11-20-r20.json), and
[independent GIF output](../evidence/gif-independent-2026-11-20-r24.json), and
[host-state execution](../evidence/host-restore-2026-11-20-r26.json).
Offscreen native/source review evidence does not establish real-DAW, optimized
performance, TSan, leak, OS-I/O deadline or physical-model qualification. Local
leak detection was disabled under the tracer. Models remain synthetic and
filesystem operations retain the documented trusted-directory assumption.

## Build-gate qualification

[Dependency verification](../audits/DEPENDENCY_SOURCE_GATE_2026-11-20_R25.md)
now checks all pinned source roots/submodules and their current contents before
upstream configuration. The [build-gate source map](../evidence/commit-map-build-gates-2026-11-20.json)
records verified local/remote tree equality. Source-only CI caching and fresh release acquisition
exclude cached population scripts. Root configured the actual native project and
executed the registered 24-case gate successfully; the six native targets then
required no rebuild. See the [execution record](../evidence/build-gates-2026-11-20-r25-r26.json).

[One Windows validator](../audits/RELEASE_VALIDATOR_GATE_2026-11-20_R25.md)
now serves CI and release, waits for completed processes, requires both products
and fresh nonempty reports, and retains failure logs. Its 16-case synthetic
PowerShell harness is installed in both workflows but has not run locally because
PowerShell is unavailable. Exact-source Windows and real pluginval remain open.

## Next continuation

1. Read the live PR #16 head and exact-head CI; repair concrete failures without
   relaxing assertions or acceptance gates. Use an expected old SHA and forward
   ref update, preserving concurrent work and original branches.
2. Complete fresh R33 aggregate reviews and independent continuation/rejection
   tests, execute the current native gate, and publish the qualified checkpoint.
   Preserve validation-before-mutation, preparation/reset identity, pending-setting
   history and explicit audio/map exclusions. The component tests already pass;
   do not relabel that result as aggregate behavior or full G12 completion.
   The host restoration repair remains complete at its recorded source.
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
