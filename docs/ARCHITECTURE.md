# CircuitDrift implementation architecture (v0.1 build-out)

Stable contract for parallel implementation. Specs remain normative (docs/planned-features); this file fixes module boundaries, ownership and cross-module interfaces. Physical models are **synthetic fixtures** (G01 open) and must be labelled as such everywhere.

## Targets and dependency direction

```
cd_core (registry, JSON) <- cd_dsp (solver, thermal, OS, protection)
   <- cd_engine (circuit runtime)      <- cd_runtime (framework-independent processor) <- plugin (JUCE adapters/UI)
   <- cd_control (macros/musical/detectors) <-'
   <- cd_state (documents, queue, A/B, history, presets) <-'
   <- cd_blueprint (graph, compiler, timeline, faults; depends on cd_engine types) <-'
   <- cd_media / cd_network / cd_community (worker-side services) <-'
```

No core target depends on JUCE, UI, OS GUI APIs, sockets or a cloud SDK. `cd_runtime` is the only place that combines control+engine+state per audio block; the JUCE plugin is a thin adapter over it.

## Directory ownership (one writer per directory during a wave)

| Directory | Target | Wave-1 owner |
|---|---|---|
| src/dsp (edits), src/engine, tests/engine | cd_dsp, cd_engine | engine agent |
| src/control, tests/control | cd_control | control agent |
| src/state (new files; existing StateDocument API stays compatible), tests/state | cd_state | state agent |
| src/media, src/network, src/community, tests/{media,network,community} | cd_media, cd_network, cd_community | services agent |
| plugin/, cmake/Dependencies.cmake, .github/workflows | plugin targets, CI | build agent |
| top-level CMakeLists.txt, cmake/CoreTarget.cmake, docs/ARCHITECTURE.md, parameters.json | coordinator | coordinator only |

Each module directory has its own CMakeLists.txt (auto-included when present). Tests live in tests/<module>/CMakeLists.txt and use `cd_add_test(<exe> SOURCES ... LIBS ...)`.

## Realtime rules (all audio-path code)

noexcept; no allocation, locks, I/O, logging, exceptions or unbounded loops after prepare; double-precision state; bounded per-sample work; block-partition invariance (any split of a buffer, including 0-frame calls, gives identical output to <=1e-9 unless a test states otherwise). Preparation (allocation, design, compile) happens on non-audio threads and is installed by pointer/generation handoff, reclaimed off-audio.

## Parameters

`src/core/ParameterRegistry.generated.inc` (from parameters.json) is the single parameter list: 74 extension controls (host IDs 1000–1999) + 21 base-engine controls per A/B slot (`slot.a.*` IDs 1–21, `slot.b.*` IDs 51–71, featureId CD-F20, origin=base). Plugin parameter string IDs are the registry `id` strings; never renamed.

A `ParamValues` snapshot is `std::array<double, core::kParamCount>` of **plain** values indexed by registry order (`cd::core::kParams[i]`). Helper lookups by id happen at prepare time, not per sample.

## Engine contract (cd_engine)

- `engine::CircuitEngine` runs both A/B slot circuits (each stereo), oversampled MNA/supply/thermal/noise per 01-runtime, then the ordered output chain: blend (level-matched) -> output trim -> HPF -> soft clip -> emergency containment. Declares integer latency; delayed dry is aligned.
- Inputs per block: `ParamValues` (plain), `ModulationBlock` (below), audio in/out (double, up to 2 ch), optional sidechain (up to 2 ch), frames 0..8192, sample index/epoch.
- Exposes `TelemetryFrame` (per component temperature K, rail volts, currents, levels, generation, fallback flags) via a lock-free single-writer snapshot for the UI (read-only).
- Component identities: stable string IDs (e.g. `C_res`, `V1`, `R_load`, `T_out`) used by inspector, tooltips, faults, timeline and blueprints.

The modulation contract is `src/core/Modulation.h` (`cd::core::SlotModulation`, `cd::core::ModulationBlock`): block-rate per-slot targets plus per-sample external load current and transient preservation arrays. Both cd_control (producer) and cd_engine (consumer) include it; only the coordinator edits it.

Condition/fault overlays (timeline CD-F18, faults CD-F19) are per-component `ComponentCondition` records defined by cd_engine and produced by cd_blueprint; identity defaults change nothing.

## State contract (cd_state)

Existing `StateDocument` load/save/migrate stays the persistence format. New: SPSC parameter transaction queue (UI/host -> audio), A/B slot copy/swap transactions, 20-step undo/redo of completed transactions with byte bound, local preset library (factory + user, import/export JSON), all worker-side except the queue consumer.

## Build and release

JUCE 8.0.17-security (commit dabd5ad13cd869cba41046947fffc8e5ce5cd662) and clap-juce-extensions 3837f4a6ed2ab0ba46aba69ae5b7041c7c64ce74, fetched by pinned commit (cmake/Dependencies.cmake). Release scope: **Windows x64 only** — VST3, CLAP, Standalone for CircuitDrift and CircuitDrift LITE. AU (macOS) and AAX (G08 signing) are excluded from this release. Windows builds run on GitHub-hosted windows runners with MSVC; Linux runs the fast core gate.

## Wave-2 ownership (runtime, blueprint, UI)

| Paths | Owner |
|---|---|
| src/runtime/**, tests/runtime/**, plugin/src/PluginProcessor.*, plugin/src/PluginEntry*.cpp, plugin/src/runtime/**, plugin/src/platform/**, plugin/CMakeLists.txt, plugin/tests/test_plugin_params.cpp | runtime agent |
| src/blueprint/**, tests/blueprint/**, src/engine/**, src/dsp/**, tests/engine/**, src/state/StateDocument.*, docs/planned-features/schemas/**, docs/planned-features/examples/** | blueprint agent |
| plugin/src/ui/PluginEditor.cpp, plugin/src/ui/Education.cpp, plugin/src/ui/shell/**, plugin/tests/ui/snap_shell.cpp | UI shell agent |
| plugin/src/ui/workbench/**, plugin/src/ui/analysis/**, plugin/tests/ui/snap_workbench.cpp | UI workbench agent |
| plugin/src/ui/panels/**, plugin/tests/ui/snap_panels.cpp | UI panels agent |
| plugin/src/ui/EditorBridge.h, Theme.h (names), Education.h, PluginEditor.h, workbench/WorkbenchView.h, panels/Panels.h, plugin/tests/ui/FakeBridge.h | coordinator (contracts) |

UI code talks to the processor only through `EditorBridge`; UI snapshot apps (`plugin/tests/ui/snap_<area>.cpp`, auto-registered) render headlessly against `FakeBridge` to PNG under `build*/plugin/ui-snapshots/`.
