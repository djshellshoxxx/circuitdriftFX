# Qualified-child integration candidate: PR #17 detector + PR #18 preset guards — 2026-11-20 UTC

## Source authority and isolation
- Parent coordinator: draft PR #16, exact `b3e05b652147a584ad5fb22e5715d1e61b702318`.
- PR #17: exact `d12fd60d83b6c56f9941ca1dc804c5bcd14ce6b2`.
- PR #18: exact `86658cf6b126e2eaabb2bac686148b76d7e7cbaa`.
- Combination uses PR #18 as parent and imports PR #17's three production/test blobs and original focused audit. The source path diff has no overlaps with PR #18 outside the handoff and state metadata. PR #16 and older branches are unchanged.

## Verified separate-head CI
| Source | Linux | Windows/pluginval |
|---|---|---|
| PR #16, [run 44690495993](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993) | Passed | Passed incl. VST3 strictness 5 |
| PR #17, [run 44694338052](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44694338052) | Passed | Passed incl. VST3 strictness 5 |
| PR #18, [run 44702393167](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44702393167) | Passed | Passed incl. VST3 strictness 5 |

The combined tree is a *new exact head* and does not inherit those passing statuses.

## Scoped source review (single execution, not independent reviewer signoff)
- Detector direct preparation validates finite sample rate and bounded ring request before conversion/doubling. Idle reset increases generation and clears publication; regression validates same-rate reprepare, rejected reprepare and reacquisition.
- Preset library integration remains message-thread/local filesystem only. Sound load/save correctly fail until atomic seed+blueprint transactions exist. No overwrite remains only a serialized-writer contract; malicious filesystem replacement, permissions, and Windows Unicode paths need dedicated defensive review.
- Scene index guards reject `-1`, `2` and `INT_MAX` with `std::out_of_range` ahead of `Config::slots` indexing. These APIs are non-audio operations and should not be moved into an audio callback.
- PNG encoding is completed before atomic destination replacement; existing destination remains subject to the documented trusted-directory local file contract.
- No independent workers could be launched through this GitHub-only connector environment. No ThreadSanitizer, live DAW, physical calibration, or release qualification is asserted.

## Mandatory next gates
1. Run Linux core, native adapter/UI suites, Windows CLAP/VST3/Standalone packaging and pluginval on the combined commit.
2. Independent code reviewer: inspect mixed preset/editor/engine state ownership, APIs, exceptions and lifecycle.
3. Independent bug hunter: exercise seek/reprepare race assumptions and file invalidation/import paths, including Windows Unicode paths.
4. Independent defensive security reviewer: examine symlink/rename assumptions, bounded memory/file inputs and no-overwrite behavior.
5. If all pass, compare live PR #16 head and integrate with a guarded non-forced forward update. Do not blanket-merge older incompatible PRs.

This is reviewable source integration only. No release, deployment or destructive rewrite.
