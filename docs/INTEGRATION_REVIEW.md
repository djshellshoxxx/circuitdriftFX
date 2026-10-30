# Core integration review

Review date: 2026-10-30. Findings refer to the exact source snapshots below; later commits require a fresh diff review.

## Branch comparison

| Branch / PR | Reviewed SHA | Finding |
|---|---|---|
| main | `c10addecb48849d64890864f65f6ae231869be53` | Contains merged PR #1 scaffold and PR #3 fixed 2x halfband stage. |
| claude/circuitdriftfx-implementation-dknarc / #2 | `c3b73d95b117b979be0c761573c49046a3df6e9e` | Draft, GitHub reports conflict-free; handoff incorrectly says oversampling is absent. Refreshed in this integration. |
| claude/funny-meitner-drdlot / #3 | `7f395066f48a49da675cf36ab7461c554ef10d4e` | Already merged; its implementation and test are retained. |
| claude/zen-cannon-v5mb75 / #4 | `a9c633fd89585a61780f206e303fced1c79326d1` | Draft; 3 commits ahead and 2 behind current main. Source review found safety defects despite successful Linux CI. |

Independent three-way file comparison found conflicts in CMakeLists.txt, docs/CHECKPOINT.md and tests/test_main.cpp. Main and PR #4 also define different classes named `cd::dsp::Halfband2x`; including both headers reproduces a compiler redefinition error. The integration retains main's fixed stage and API, names PR #4's configurable polyphase stage `PolyphaseHalfband2x`, and runs both original acceptance suites. No workflow/release changes are needed.

## Review findings and fixes

| Area | Reproduced defect at PR #4 | Correction / acceptance |
|---|---|---|
| JSON number locale | With `LC_NUMERIC=de_DE.utf8`, parse `1.25` accepts value 1; writing 1.25 emits invalid JSON `1,25`. | Locale-independent numeric conversion and regression coverage. |
| Local save | A pre-existing `<path>.tmp` symlink causes a successful save to overwrite a different file. | Exclusively create a unique same-directory temporary file, check I/O completion, then replace the destination. Test replacement, existing temporary/symlink preservation and failures. |
| Publication | Nested synthetic credentials/path/audio fields survive scene export through opaque configuration objects. | Validate allowed publication fields; reject unsupported sound-bearing payloads and keep caller output unchanged on failure. |
| Parameter IDs | `hpf.frequency\u0000alias` is accepted as `hpf.frequency` by C-string lookup. | Reject NUL-bearing identifiers before registry lookup. |
| Integer identities | Seed 9007199254740993 is accepted as 9007199254740992 after double rounding. | Preserve enough parse provenance to reject rounded integer identities. |
| Feature identity | Required feature CD-F20 is rejected because it lacks a parameter registry entry. | Recognize feature IDs independently from the parameter registry. Recognition does not claim full feature implementation. |
| Oversampling bounds | Valid-syntax extreme FIR edge overflows a float-to-int cast; tiny IIR pass edge prepares successfully with NaN latency/output. | Check finite intermediates and resource bounds before allocation/conversion; failed prepare cannot publish invalid processing state. |
| Thermal/protection primitives | Original thermal preparation accepts infinite ambient and overflowing C/dt; HPF accepts infinite sample rate; feeding linked powers yields gain about 0.25 instead of 0.5. | Transactional finite-state checks, explicit linked-power gain API, and primitive safety regressions; existing sample API retained. |
| Solver finite state | Extreme finite sources overflow initial capacitor charge; infinite tolerance permits nonfinite DC state. | Validate tolerance and all accepted state/charge, and preserve the active solver when preparation fails. |

## Code and security coverage

The standards review checked the current core/DSP/state implementation, generator, CMake and workflow against CLAUDE.md: dependency direction, bounded process work, finite double state, noexcept processing and truthful partial status. It found numerical validation and preparation-state defects, now covered by regressions.

The specification review checked persistence identities, locale independence, publication privacy and retained main-branch acceptance behavior. The focused security pass checked untrusted JSON depth/size/UTF-8/duplicates, quadratic duplicate detection, nested export payloads, temporary-file and destination symlinks, concurrent saves and write failures. Current production code has no network service, OAuth, IPC or host adapter to audit. Their security and host qualification remain future gates. Passing these checks is not a claim that every bug is absent or that realtime stress qualification is complete.

## Evidence before corrections

- GitHub PR #4 Linux run [41004669925](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/41004669925) succeeded, including Release CMake configure, build and CTest. This is remote evidence for the original PR head, not the reconciled tree.
- The reviewed PR #4 sources were fetched by exact SHA through the GitHub connector. Unchanged compiled sources were checked for equality with that SHA. The managed environment Git proxy could not connect; no proxy or authentication configuration was changed.
- GCC 14.2.0, C++20, `-O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer`: direct build of all PR #4 core/DSP/state sources and tests/test_main.cpp succeeded. CMake is not installed locally.
- The initial sanitizer executable exited because LeakSanitizer cannot run under this environment's ptrace restriction. Rerun with `ASAN_OPTIONS=detect_leaks=0` passed all existing assertions. AddressSanitizer and UndefinedBehaviorSanitizer stayed enabled; leak checking was not completed locally.
- Registry freshness and documentation validation passed. The latter is documentation-only evidence.
- Original T21 measurements reproduced: nominal 4x FIR −150.33 dB, 16x FIR −150.19 dB; hot 4x FIR −53.13 dB against −70 dB proposed target. The existing suite reports the hot-case miss without failing. T21 remains Partial.

## Correction verification

Executed from `/workspace/circuitdriftFX-fixes-20261030`:

```sh
python3 /tmp/circuitdriftfx-integration-build/verify.py
```

The verification script compiles all nine `src/**/*.cpp` translation units and five `tests/test_*.cpp` files with GCC 14.2, C++20, `-O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -Isrc`, links each test to those production objects, and runs with `ASAN_OPTIONS=detect_leaks=0`. It then invokes the registry and documentation validators. Source snapshot is this integration tree; the remote commit SHA is recorded in the PR.

| Executed check | Result |
|---|---|
| Combined original acceptance suite (`test_main`) | PASS; includes preserved main `t21_halfband` plus PR #4 suite and unchanged migration digest |
| Oversampling safety | PASS, 0 failures; bounds, failure/reprepare behavior, coexistence and block-partition equivalence |
| Primitive safety | PASS; thermal/HPF finite-state rejection and linked mean-square gain |
| Solver safety | PASS; finite state/charge, checkpoint rejection, failed-prepare preservation and recovery |
| State safety | PASS, 0 failures; three comma-locale selections, 20,000-key input, exact IDs, publication, symlinks, concurrent saves and forced write failure |
| `python3 tools/gen_param_registry.py --check` | PASS |
| `python3 tools/validate_planned_features.py` | PASS; documents/examples only |

The integration CMake registers these as seven CTest entries. Local CMake/CTest itself was unavailable; consult GitHub checks on the integration commit for that separate result. Source review confirmed the existing workflow runs only Linux configure/build/test with read-only contents permission and no release/deployment step.

Windows file replacement is implemented but unexecuted here. Atomic save requires a trusted destination directory and does not promise directory-entry durability after power loss. Publication currently accepts only the documented minimal synthetic subset and rejects unsupported configuration rather than changing its sound. No synthetic check qualifies physical hardware, complete audio callback safety, Windows/macOS hosts, licensing or release readiness.
