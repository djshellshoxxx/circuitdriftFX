# Live editor and runtime continuation validation — 2026-11-20 UTC

## Qualified source

The final production/test change in the combined core run is local commit
`1f21fe95a2d495be8abe797437730577575f9d58`, published with the identical tree as
`581745655f26c51fb1b092fe4db9e32a035e1f0c`. Its tree is
`8394d34c4c0e15900e77188332cec1b37d2621af`.
The later local `9d8ba78e31e232a097aa3ae18a74de9ff7b5cb9b` / remote
`d6d20df93bfc13eeb98874b3489b8030f5aaef23` adds audit documentation only.
The [commit mapping](../evidence/commit-map-2026-11-20.json) preserves each reviewed
local source and its tree-identical remote commit. Publication recreated Git
commit metadata; it did not alter the reviewed file trees.

## Results

| Check | Result | Scope |
| --- | --- | --- |
| Combined portable core | **30/30 passed**, 127.89 s | GCC 14.2, C++20, `-O1 -g0 -DNDEBUG`, ASan, UBSan, float-cast-overflow |
| Repaired parameter queue | **25/25 repeated runs passed**, 4.52 s | Separate review worktree, same queue production/test content; includes deterministic ordering, coherent records, final-value and allocation assertions |
| Native processor consistency | **Passed**, 4.03 s | Clang 19.1.7, Custom `-O0 -g0 -DNDEBUG`, pinned JUCE; real adapter, session preferences, reset, telemetry lifecycle and gestures |
| Plugin generated tables | **Passed** | `plugin/tools/gen_plugin_tables.py --check` |
| All five native UI suites | **5/5 passed** | Live bridge, shell, Workbench, panels and editor contract; together with processor consistency, **6/6 native suites passed** |

The [core result JSON](../evidence/core-ctest-2026-11-20.json) lists all 30 suites.
The [complete core CTest log](../evidence/core-ctest-2026-11-20.log) retains output,
including adverse alias measurements and benchmark limitations. The
[native result JSON](../evidence/native-ctest-2026-11-20.json) and
[native CTest log](../evidence/native-ctest-2026-11-20.log) record all six executed
suites across five CTest invocations. Absolute scratch paths are replaced by stable
`<checkout>`, `<build-core>`, `<build-clang>` and `<workspace>` placeholders only.

The first combined run failed the existing `state_queue` monotonicity assertion
(29/30 passed). Review found a real ordering defect between the bounded ring
snapshot and coalesced updates, plus incoherent publication of value/gesture
fields. The repair transfers complete events through three fixed SPSC slots and
uses per-parameter sequence numbers to reject superseded ring entries. The
original assertions remain. The final combined pass above includes the repair;
it is not a selected retry of unchanged failing code.

## Commands and environment

Core:

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DCD_SANITIZE=ON \
  -DCMAKE_CXX_FLAGS_RELEASE='-O1 -g0 -DNDEBUG'
cmake --build build-core --parallel 1
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-core \
  --output-on-failure --parallel 2 --output-junit core-results.xml
python3 plugin/tools/gen_plugin_tables.py --check
```

Native tests use the pinned sources in `cmake/Dependencies.cmake`, including JUCE
`dabd5ad13cd869cba41046947fffc8e5ce5cd662`. Linux development libraries and Clang
were extracted into an unprivileged sysroot. A local environment wrapper supplies
the include/library/pkg-config paths; repository CI settings were not changed.

```sh
CC=clang CXX=clang++ cmake -S . -B build-clang -DCD_BUILD_PLUGIN=ON \
  -DCD_DEPS_CACHE_DIR=deps -DCMAKE_BUILD_TYPE=Custom \
  -DCMAKE_CXX_FLAGS_CUSTOM='-O0 -g0 -DNDEBUG' \
  -DCMAKE_C_FLAGS_CUSTOM='-O0 -g0 -DNDEBUG'
cmake --build build-clang --target cd_plugin_consistency_tests \
  cd_ui_snap_live_bridge cd_ui_snap_shell cd_ui_snap_workbench \
  cd_ui_snap_panels cd_ui_snap_contract --parallel 1
ctest --test-dir build-clang -C Custom --output-on-failure \
  -R '^plugin_param_consistency$' --output-junit native-processor-results.xml
ctest --test-dir build-clang -C Custom --output-on-failure \
  -R '^ui_snap_live_bridge$' --output-junit native-live-bridge-results.xml
ctest --test-dir build-clang -C Custom --output-on-failure \
  -R '^ui_snap_shell$' --output-junit native-shell-results.xml
ctest --test-dir build-clang -C Custom --output-on-failure --parallel 2 \
  -R '^(ui_snap_workbench|ui_snap_panels)$' --output-junit native-workbench-panels-results.xml
ctest --test-dir build-clang -C Custom --output-on-failure \
  -R '^ui_snap_contract$' --output-junit native-contract-results.xml
```

The UI suite durations were: live bridge 0.22 s, shell 19.69 s, Workbench
12.34 s, panels 10.57 s and contract 0.83 s. Tests draw offscreen; no interactive
display server or native DAW was used. Representative Easy and Workbench PNGs
were visually inspected. Their telemetry is the labelled synthetic fixture.

LeakSanitizer is disabled only for local sanitized execution because this
environment runs under a tracer. No leak-check result is claimed. Initial GCC
native compilation exceeded local memory; the successful native check uses
Clang and `-O0`. Native checks are not sanitizer or optimized performance runs.
An ordinary compiler cache is used for repeated identical test-support builds,
with no unsafe cache relaxation.

## Windows CI attribution

The previous exact PR #16 head
`cb53cd812700e5edbcf7323653b84e470da77cfc` completed
[run 44674618903](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44674618903)
successfully: Linux job `136083378151` and Windows job `136103454565` both passed.
Windows completed its build, CTest, VST3/CLAP/Standalone staging, package upload
and VST3 pluginval at strictness 5. The earlier PowerShell waiting/exit-status
repair is therefore executed evidence on that source.

That result does **not** qualify this later live-editor continuation. Read the
checks for the new exact PR head after publication. The workflow retains exact
head checkout, all formats and original acceptance criteria. No merge, release
or deployment is included in this checkpoint.

## Practical boundaries

The new coverage exercises real APVTS edits and scoped undo, reset requests at
audio boundaries, worker-owned pitch invalidation, persistent editor settings,
host dirty notification, telemetry generations and the repaired bounded state
queue. Open-editor restoration, immediate resize, PCB view persistence and
gesture/layout contracts passed in the native UI suites. A focused stack-size
inspection found no confirmed blocking regression; MSVC qualification remains
part of the new Windows run. This does not establish a measured realtime reset deadline, a one-hour
native host qualification, physical calibration or release CPU budgets.

Dynamic DSP/seed session continuity, atomic dynamic-state A/B transfer, live
preset/blueprint/fault services, provider/host IPC, native GIF/MP4 and older-branch
semantic reconciliation remain tracked in the
[current handoff](CONSOLIDATION_HANDOFF.md). A/B copy/swap currently changes
parameters. Synthetic fixtures and adverse ungated alias cases remain labelled;
no threshold was weakened to obtain the pass.
