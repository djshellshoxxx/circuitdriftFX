# ADR-0001: Plugin build, formats, licensing and host parameter ids

Status: Proposed (closes G09 only after the owner licence decision and the id audit in section 3 are confirmed on a real fetch)
Scope: Windows x64 release line. Gates touched: G09 (JUCE pin, licence, CLAP adapter). G08 (AAX) stays open and out of scope.

## 1. Decision

| Item | Choice |
| --- | --- |
| Framework | JUCE tag `8.0.17-security`, commit `dabd5ad13cd869cba41046947fffc8e5ce5cd662` |
| CLAP adapter | clap-juce-extensions (MIT) commit `3837f4a6ed2ab0ba46aba69ae5b7041c7c64ce74` |
| CLAP headers / helpers (cje submodules) | `195b42a004144fab0b3cf95e9c067187d15365b7` / `a380c978f429103e08afc1d17e9efbf3e3c96ee2` |
| Formats shipped | VST3, CLAP, Standalone, for two products: CircuitDrift (full) and CircuitDrift LITE |
| Formats excluded | AU (macOS only, not in a Windows-only release), AAX (gate G08: SDK access, signing, retail-host validation), LV2/VST2 (not planned) |
| Toolchain | MSVC (Visual Studio 2022 generator), x64, Release, C++20, static MSVC runtime |

Pins live in `cmake/Dependencies.cmake`. They are git commit SHAs, which are content addressed, fetched through
`FetchContent` (git) and re-verified with `git rev-parse HEAD` at configure time, including the two cje submodules
(taken from the cje commit, then checked against the SHAs above). Sources and FetchContent stamps live under
`CD_DEPS_CACHE_DIR` so CI can cache them keyed on `hashFiles('cmake/Dependencies.cmake')`.
Archive SHA-256 hashes were not used because the pinned archives could not be downloaded when this scaffold was
written; if a tarball based fetch is preferred later, compute the hashes from the pinned commits and add `URL_HASH`.

Updating a pin is a deliberate change: edit `cmake/Dependencies.cmake`, update this ADR and
`THIRD-PARTY-NOTICES.md`, re-run the id audit (section 3) and the plugin consistency test.

## 2. Licence (decided 2026-11-18)

The owner chose **AGPL-3.0-or-later** for CircuitDrift (root `LICENSE`), and JUCE 8 is used under its AGPLv3
option. Distributed binaries (full and LITE) are offered with complete corresponding source in the public
repository at the release tag. Components compiled into the binaries are permissive and AGPL-compatible:
clap-juce-extensions, CLAP and clap-helpers (MIT) and the Steinberg VST3 SDK bundled in the pinned JUCE tree
(`modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt`: MIT, Steinberg 2025). pluginval (GPLv3)
is a CI tool only and is never distributed. A closed-source distribution would require a JUCE commercial licence and
a new decision recorded here.

## 3. Host parameter id audit

Why this matters: `parameters.json` states that wrapper ids must use an explicit stable mapping and that
`hostId` (1000-1999) is permanent. Automation recorded in a project must keep working across plugin updates.

### 3.1 What the adapter does

* Every registry parameter becomes a JUCE parameter whose **string id equals the registry id** (for example
  `macro.age`), version hint 1 (`cd::plugin::kParameterVersionHint`). Hidden or GUI-only items are not
  parameters at all.
* Full edition exposes every registry entry except `lite.*`; LITE exposes only `lite.*` plus a wrapper `bypass`
  parameter. The two products have different `PLUGIN_CODE` (`Cdfx` / `Cdlt`), the same manufacturer code (`Cdrf`),
  different bundle ids and different CLAP ids, so format level identifiers cannot collide.
* `automatable = false` registry entries are exported as non-automatable JUCE parameters.

### 3.2 How the pinned wrappers derive host ids

Audited against the pinned tree (JUCE 8.0.17-security `dabd5ad`, clap-juce-extensions `3837f4a`) on 2026-11-18:
`juce_audio_plugin_client_VST3.cpp` maps each parameter with
`VST3ClientExtensions::convertJuceParameterId(juceParamID, JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS)` (default 1:
`String::hashCode() & 0x7fffffff`), or the index when `JUCE_FORCE_USE_LEGACY_PARAM_IDS=1` (default 0, not enabled);
`clap-juce-wrapper.cpp` uses `static_cast<uint32_t>(LegacyAudioParameter::getParamID(param, false).hashCode())`.
`plugin/tests/test_plugin_params.cpp` freezes the consequences (golden table) so any drift is detected.

* **VST3.** `juce_VST3_Wrapper.cpp` derives `Vst::ParamID` from the JUCE parameter id string with
  `juce::String::hashCode()` (a 31-multiplier hash over int32, cast to the unsigned 32-bit ParamID). With
  `JUCE_FORCE_USE_LEGACY_PARAM_IDS=1` the wrapper instead uses the parameter *index* (as a number), which is worse for
  stability because reordering changes ids. The Studio One compatibility switch
  (`JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS`, if present in the pinned tree) only affects the integer form of the id;
  the project does not enable the legacy mode.
* **CLAP.** clap-juce-extensions generates `clap_id` from the same JUCE parameter id (hash of the string id), also
  honouring the legacy-index fallback for parameters without a string id.
* **Consequence.** The ids hosts see are a deterministic function of the registry string id. They equal neither the
  registry `hostId` nor each other across formats unless the wrappers hash identically. JUCE offers no documented
  mechanism to force a VST3 ParamID to an arbitrary integer while keeping string ids, and making the JUCE string id
  the decimal `hostId` would only be honoured in the legacy mode that is keyed on index, so it was not adopted.

### 3.3 Stability guarantees

1. A registry string id, once published, is never renamed or reused; to change meaning, add a new parameter.
2. `hostId` stays permanent in the core registry and is the key for the project state format (`cd_state`); wrapper ids
   are a derived view.
3. The test `plugin_param_consistency` fails if any registry id, its `hostId`, or the hash derived from it changes
   (`plugin/tests/ParamIdGolden.generated.inc`, generated by `plugin/tools/gen_plugin_tables.py`, freshness checked by
   `plugin_tables_fresh`), if two ids hash to the same 32-bit or 31-bit value, if a registry parameter is missing from
   the JUCE tree, or if kind, range, log mapping, default, automatable flag or enum labels differ from the registry.
4. Changing `JUCE_FORCE_USE_LEGACY_PARAM_IDS`, `kParameterVersionHint` semantics or a wrapper (JUCE/cje pin) requires
   re-running the audit and an ADR update, because it can change host ids.
5. A future explicit hostId-to-wrapper-id map (if the audit finds a supported override in the pinned JUCE) must keep
   every previously shipped hash as the host id, i.e. it is allowed only as a compatibility layer.

## 4. Adapter behaviour (current)

* `CircuitDriftProcessor` is a pass-through shell. DSP (`cd_dsp`), state (`cd_state`) and the real GUI are not wired
  yet; the editor is JUCE's generic editor and state is APVTS XML, an interim format. Do not describe the plugin as
  processing audio.
* Buses: main in/out mono or stereo (input layout equals output layout), plus an optional sidechain input (disabled by
  default; mono or stereo when enabled). MIDI input is accepted for key tracking. Double precision is supported.
* Compile definitions on all JUCE targets: `JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`, `JUCE_DISPLAY_SPLASH_SCREEN=0`,
  `JUCE_REPORT_APP_USAGE=0`, `JUCE_VST3_CAN_REPLACE_VST2=0`, `JUCE_STRICT_REFCOUNTEDPOINTER=1`,
  `JUCE_MODAL_LOOPS_PERMITTED=0`.
* Static MSVC runtime (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`) is required for the whole
  tree and enforced by `plugin/CMakeLists.txt`; core libraries, tests and plugins must agree.

## 5. Consequences and open items

* Windows execution is not qualified by this scaffold: it was written without running a Windows build. CI results of
  the first push are the evidence.
* pluginval (GPLv3) is used only as a CI tool and is not distributed. Its release asset hash must be filled in
  `release.yml` before the first release (the workflow refuses to run with the placeholder).
* No CLAP validator is pinned; CLAP receives a structural smoke check (entry symbol and bundle layout) only.
* G09 is closed only when: the owner licence choice is recorded, the section 3.2 audit is confirmed against the
  fetched pinned sources, and a Windows CI run passes.
