# R14 integration and live validation evidence — 2026-11-20

## Qualified source and provenance

Final production/test source is local
`b78430cd941201be382a09809ca40be997e959c1`, reproduced remotely as
`e2d91a45d6a45ab8987622a00a19fbed1049e7e6`. Both have tree
`8bad08aa4a5bca5b2ef796536cd1ceff3806d797`. The
[commit mapping](../evidence/commit-map-2026-11-20-r14.json) records all eleven
new commits and verifies each remote tree against its local tree. The containing
checkpoint adds evidence and handoff documentation after that source.

Nine continuation branches form the nineteen-commit chain ending at
`c6ebb22daab9c46edffa761b21f3fd45320e5074`. The separate pitch-preparation branch's
four files were already imported byte-identically into that chain. The
[integration audit](../audits/R14_INTEGRATION_REVIEW_2026-11-20.md) records every
branch/head and the equivalence check. Original branches and older PRs remain
preserved; this does not close the older PR #9/#10/#14 semantic review.

## Executed results

| Check | Result | Exact boundary |
| --- | --- | --- |
| Combined portable core | **30/30 passed**, 335.45 s | GCC 14.2, `-O1 -g0 -DNDEBUG`, ASan/UBSan/float-cast-overflow, parallel 2; leak detection disabled under the tracer |
| Native processor/editor suites | **6/6 passed**, 25.23 s wall time | Clang 19.1.7, Custom `-O0 -g0 -DNDEBUG`, pinned JUCE, parallel 2, sanitizers off |
| Generated parameter registry | **Passed** | `python3 tools/gen_param_registry.py --check` |
| Generated plugin tables | **Passed** | `python3 plugin/tools/gen_plugin_tables.py --check` |
| Visual inspection | **Passed for the inspected final dark Blueprint render** | JSON text readable after theme switching; automated panel matrix also checks both themes, scales and narrow layouts |

The core run used local `4ae50007c512f02202b966f239fbdfbc25c2c2ce`, reproduced as
`5fd0a291f831bc05a39a3ac9f2eef45be32df93f`. The only later source change is five
lines in `plugin/src/ui/panels/LifecyclePanels.cpp` to recolor stored text when the
theme changes. A zero-diff comparison of portable source/build inputs confirmed
that the core tested above is unchanged. All six native suites were rebuilt and
rerun after that UI repair, on the final source named at the top.

The [core JSON](../evidence/core-ctest-2026-11-20-r14.json) lists all thirty results;
the [complete core log](../evidence/core-ctest-2026-11-20-r14.log) retains numeric
output and benchmark qualifications. The [native JSON](../evidence/native-ctest-2026-11-20-r14.json)
and [complete native log](../evidence/native-ctest-2026-11-20-r14.log) record the
final six-suite invocation. Workspace paths are normalized; results are not
selected from repeated runs of an unchanged failure.

| Native suite | Seconds |
| --- | ---: |
| `plugin_param_consistency` | 4.20347 |
| `ui_snap_contract` | 0.996743 |
| `ui_snap_live_bridge` | 1.53543 |
| `ui_snap_panels` | 11.3085 |
| `ui_snap_shell` | 19.1515 |
| `ui_snap_workbench` | 12.9155 |

## New behavior covered

- Real asynchronous Blueprint compilation, accurate diagnostics and cost/configuration
  fields, UTF-8 request size admission, latest-request replacement, explicit
  cancellation, one-time report delivery, pending host/configuration invalidation,
  unchanged persisted host state/history, worker shutdown and LITE rejection.
- Production Blueprint panel integration, the synthetic Example RC draft,
  cancellation on edit/reload/slot change/destruction, and unavailable Apply
  remaining disabled even after successful validation. Tracking bridges remain
  clearly identified UI fixtures; the live suite uses the actual compiler.
- Preset selection following stable identities across filter/reorder/removal,
  explicit double-click targets, factory protection, and visible catalog errors.
- Preset directory work bounds before query filtering, duplicate parameter
  rejection during publication, and state writer resource admission before
  recursive copies, including exact depth/byte and private-field stripping cases.

See the [feature note](BLUEPRINT_VALIDATION_2026-11-20_R14.md),
[worker cancellation review](../audits/R14_BLUEPRINT_CANCELLATION_REVIEW_2026-11-20.md),
[preset selection review](../audits/PRESET_SELECTION_REVIEW_2026-11-20_R14.md),
[catalog/publication review](../audits/CATALOG_PUBLICATION_GUARDS_2026-11-20.md), and
[writer resource review](../audits/STATE_WRITER_RESOURCES_2026-11-20_R14.md).
Independent source reviews found no remaining blocker in their recorded scopes.
The final theme fix was also independently checked against pinned JUCE: recoloring
does not emit text-change notifications or create undo transactions.

## Build recoveries and limits

The first concurrent core/native build exhausted compiler memory in
`tests/test_main.cpp`. The existing `-O1` core configuration succeeded when rebuilt
after native compilation; no acceptance criterion or CI flag was relaxed. A new
preset UI test initially called an unavailable `juce::StringArray::getLast()`;
supported indexing replaced those calls while retaining their assertions.
The first six-suite native pass was followed by visual inspection, which found
the theme/text contrast defect. The final six-suite run above follows its repair.
Native output includes `std::filesystem::u8path` deprecation warnings.

The first part of the core test run overlapped native compilation. Its wall time
is recorded as execution evidence, not a throughput benchmark. These are portable
sanitizer tests and offscreen native console applications. They do not establish
leak-check, ThreadSanitizer, real-DAW, physical-model or production-performance
qualification. Blueprint Apply, complete sound-preset transactions, dynamic
session/seed continuation and the other documented service gates remain open.

## Remote CI boundary

Two previous exact sources have completed Linux, Windows and pluginval:

- PR #16 source `b3e05b652147a584ad5fb22e5715d1e61b702318`:
  [run 44690495993](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44690495993).
- Inherited chain tip `c6ebb22daab9c46edffa761b21f3fd45320e5074`:
  [run 44764678463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44764678463).

Those results qualify their own sources only. The containing R14 publication
requires its new exact-head Linux/Windows/pluginval run; inspect the live
[PR #16 checks](https://github.com/djshellshoxxx/circuitdriftFX/pull/16).
No merge, release or deployment is authorized or performed by this checkpoint.
