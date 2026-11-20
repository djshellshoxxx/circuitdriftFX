# Consolidated validation — 2026-11-19

## Qualified source and result

Local source commit `f7c7b6a989438b7f320a191a7b63c2ccffa2a3b9` (tree `9391285fde4fa66861aebb40ccf1e3f7de7c054a`) passed **30/30 core suites** in
**136.40 seconds**. The final publication adds this evidence and documentation;
no production source changed after that successful run.

The [machine-readable results](../evidence/core-ctest-2026-11-19.json) list every
suite and duration. The [complete local CTest log](../evidence/core-ctest-2026-11-19.log)
retains test output, including adverse alias measurements and benchmark limitations.
The absolute checkout path is replaced with `<checkout>` only.

## Executed commands

GCC 14.2.0, CMake 4.4.5, C++20:

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG -g0' \
  -DCMAKE_C_FLAGS_RELEASE='-O1 -DNDEBUG -g0' -DCD_SANITIZE=ON
cmake --build build-core --parallel 1
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-core --output-on-failure --parallel 2
python tools/gen_param_registry.py --check
python plugin/tools/gen_plugin_tables.py --check
python tools/validate_planned_features.py
```

ASan, UBSan and float-cast-overflow checks were enabled. LeakSanitizer was disabled
locally because it cannot operate under this environment's tracer. This is not a
leak-check result. CI retains the repository's normal sanitizer settings.

## What the run covers

Core JSON/registry/state safety; numerical primitives and solver; engine devices,
checkpoint continuation/rejection, nominal alias criteria and stress containment;
control macros, pitch, key, tempo, harmonic, dynamics and snapshot/clock safety;
state queue/history/presets; recovered blueprint compiler/lifecycle/fault suites;
media, cache/community, bundle/network; runtime/audio integration and numeric UI
input. The runtime suite includes zero callback allocations, oversized blocks,
MIDI ordering/overflow, prepared-mode staging and zero-frame note release. The
control suite checks that zero-frame events preserve drift/RNG and tempo clocks.

## Plugin and CI status

Final `PluginProcessor.cpp` and `test_plugin_params.cpp` passed `g++ -std=c++20
-fsyntax-only` against pinned JUCE `dabd5ad13cd869cba41046947fffc8e5ce5cd662`.
The fresh reviewer also syntax-checked all affected UI implementations and related
consistency/snapshot tests. No full local JUCE link, executed GUI or host run is
claimed; prior full builds exceeded memory.

Draft [PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16) is the native
CI handoff. Its initial run was queued when this record was written. Always read
checks for the latest live head; an older run is not qualification of a new commit.
Windows parameter tests, UI snapshots, format builds, staging and pluginval remain
required before native qualification.

## Limits preserved from actual output

All electrical fixtures remain synthetic. Passing tests do not establish physical
calibration, a one-hour host stress qualification or measured release performance.
The T21 log retains ungated adverse cases: for example the hot full-engine Render
published-window result is -78.43 dB against the proposed -85 dB criterion, while
its settled-window result is -101.34 dB. Other extreme stimuli also exceed proposed
criteria. These findings are retained; no acceptance threshold was weakened.
Sanitized benchmark timings are explicitly nonrepresentative of production CPU cost.

Complete session/seed migration, safe seek coordination with the pitch worker,
a real custom editor bridge, actual service/provider/native-media integration and
remaining older-branch reconciliation are tracked in the
[continuation handoff](CONSOLIDATION_HANDOFF.md).
