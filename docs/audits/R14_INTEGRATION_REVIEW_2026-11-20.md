# R14 continuation integration review — 2026-11-20 UTC

## Scope and result

Independent read-only source and ancestry review of the ten newer continuation
branches fetched after PR #16 source
`b3e05b652147a584ad5fb22e5715d1e61b702318`. Read `CLAUDE.md`, the production/test
diffs, their call sites, and the inherited audit/handoff records. No build, test
execution, source modification, remote ref update or release was performed by
this reviewer. This note is the only file written.

**Integration result:** nine branch heads form one linear descendant chain. Its
tip `c6ebb22daab9c46edffa761b21f3fd45320e5074` contains 19 commits and changes
23 paths relative to the qualified PR #16 head. A single fast-forward onto an
isolated integration branch preserves the actual history and avoids repeated
cherry-picks. The coordinator performed that local fast-forward on
`codex/cdfx-integration-r14-20261120`; remote publication remains separate.

## Exact branch provenance and integration order

All branch names below have the `codex/` prefix. Rows are ancestor order; they
are not nine additional merge operations to perform after selecting the tip.

| Branch | Exact head | Added scope |
| --- | --- | --- |
| `cdfx-preset-services-20261120-r4` | `1f1a92a5cc8d6b44a863410c2967a8d44b6132c8` | Live local catalog/import/export/delete and atomic PNG output |
| `cdfx-preset-guard-20261120` | `86658cf6b126e2eaabb2bac686148b76d7e7cbaa` | Reject invalid scene slot indices before array access |
| `cdfx-qualified-children-20261120` | `e9c00c51cdf155127cd839c446126ae1d8f9f6c6` | Integrate the separate pitch preparation repair |
| `cdfx-preset-atomic-create-20261120` | `8deb187da5cb8e0428b61d50b4a19be7307e6385` | Enforce no-overwrite at the final filesystem install |
| `cdfx-session-checkpoint-guard-20261120` | `0487a63d07e58a228a32705f03272efacc8fdba9` | Include RNG layout and finite dynamic values in restore admission |
| `cdfx-restore-ci-bounds-20261120-r10` | `7a95379554aba5d3d42f441436e58cae43ddec4b` | Correct prepared RNG fixture and reject oversized checkpoint vectors |
| `cdfx-session-writer-preflight-20261120-r11` | `b9994290a8db03b50535f597ab76649e4b347b2f` | Preflight directly assembled checkpoints before serialization |
| `cdfx-writer-roundtrip-20261120-r12` | `b6a1acdf653590126416fd0229a94a40c3dd9117` | Whole-envelope writer/reader symmetry and regression compile correction |
| `cdfx-scene-cardinality-20261120-r13` | `c6ebb22daab9c46edffa761b21f3fd45320e5074` | Reject invalid document kind and lossy scene/session blueprint cardinality |

The tenth branch, `codex/cdfx-pitch-prepare-bounds-20261120`, is the sibling
`d12fd60d83b6c56f9941ca1dc804c5bcd14ce6b2`. It is not a Git ancestor of the
combined chain because its contents were imported by `e9c00c5`. Direct comparison
against both that integration and the final `c6ebb22` tip is byte-identical for:

- `src/control/PitchDetector.cpp`
- `src/control/PitchDetector.h`
- `tests/control/test_pitch.cpp`
- `docs/audits/PITCH_PREPARE_FOLLOWUP_2026-11-20.md`

Do not cherry-pick the pitch commit again. Its original branch remains provenance.
The intermediate RNG fixture failure and unsupported metadata equality assertion
are corrected in later descendants. Selecting incomplete intermediate commits
would reintroduce known integration failures already represented in this history.

## Source review and feature boundaries

The changes preserve dependency direction and do not add filesystem operations
or exceptions to an audio callback. The preset/PNG adapter stays on the message
thread. Scene slot helpers are non-audio state APIs; their new range checks run
before indexing. All current `RuntimeIdentity` callers are tests, and the fixture
that expects exact continuation now declares its RNG layout explicitly.

The local preset feature is narrower than complete sound loading:
`ProcessorEditorBridge::listPresets`, `importPreset`, `exportPreset` and
`deletePreset` use the existing library, but `loadPreset` and `savePreset` still
report unavailable. The integration must not be described as complete live
preset application. Runtime slot seed/blueprint handoff remains missing.

The G12 branches improve `StateDocument` admission and serialization. They do
not capture the running engine/control/pitch state, persist a prepared runtime
seed through the plugin host state, or apply dynamic restoration while audio is
stopped. None adds a production caller of `decideRestore`. Exact dynamic session
continuity and coordinated dynamic A/B transfer remain unfinished.

No competing production implementation or merge conflict was found among these
ten branches. Older PR #9/#10/#14 architectures still require their separately
documented semantic comparison; this ancestry result does not cover them.

## Follow-ups passed to the current review team

Two concrete residual boundaries were identified in the inherited candidate and
confirmed by the independent state reviewer:

- `PresetLibrary::list` limits matching output count, so a filtered query does
  not bound inspected directory entries. The new synchronous UI adapter makes
  that existing library limitation user-visible. The state reviewer owns its
  enumeration-bound repair and regression.
- `saveState` performs recursive JSON copying/writing before its new reader
  roundtrip enforces depth/byte admission. Direct in-memory containers still
  need bounded pre-serialization handling. This is distinct from the new
  checkpoint-vector limits and must not be described as already solved.

The state reviewer separately found duplicate parameter IDs being collapsed by
`publishScene`; that worker owns the repair. The functional reviewer owns stale
preset selection after filtering/reordering. These current-generation repairs
require their own review and execution; this note does not pre-approve them.

## Observed CI and remaining integration gates

- The coordinator verified both platforms and pluginval for PR #16 source
  `b3e05b6`, [run 44690495993](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993).
- Direct API inspection verified PR #24 exact source `b6a1acd`,
  [run 44759057463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44759057463),
  completed successfully.
- At this review's final read, PR #25 exact source `c6ebb22`,
  [run 44764678463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44764678463),
  had passed Linux job `136386016558`; Windows job `136388482951` was in progress.

Read live status before relying on these observations. Neither parent passes
nor the running tip's result qualifies later R14 fixes. Run the affected core
and native suites on the final combined source, preserve original acceptance
criteria, review the fresh patches independently, then use a guarded non-forced
update after checking PR #16's live head. Refresh the opening handoff summary:
its appended historical entries retain earlier pending-status observations.

No release, deployment, physical calibration, realtime deadline or full native
host qualification is asserted by this source review.
