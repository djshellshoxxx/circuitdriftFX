# CircuitDrift

CircuitDrift is a planned audio effect plugin exploring dynamic circuits, component aging, temperature, power-supply sag and creative analog-inspired modulation.

**Status: engineering consolidation in progress. The source includes the circuit engine, musical controls, state/preset primitives, Windows VST3/CLAP/Standalone adapters, and a working engine-to-host runtime connection. The custom editor is still tested with a fake bridge; the live plugin currently uses JUCE’s generic parameter editor. Current binaries, host compatibility and release readiness remain subject to CI and host qualification. All circuit models are synthetic fixtures, not calibrated hardware emulations.**

## Product and UX

- [Original product vision](docs/PRODUCT_VISION.md)
- [UX design brief](docs/UX_DESIGN_BRIEF.md): PCB Workbench, Easy Mode/Tone State Sphere, direct gestures, lifecycle and analytical displays
- [Planned feature register](docs/planned-features/FEATURE_REGISTER.md): all 23 requested features

## Engineering package

- [Specification index](docs/planned-features/README.md)
- [UX interaction and display contracts](docs/planned-features/08-ux-interaction-and-displays.md)
- [Decisions and resolved overlaps](docs/planned-features/DECISIONS.md)
- [Parameter registry](docs/planned-features/parameters.json)
- [Acceptance criteria](docs/planned-features/ACCEPTANCE.md)
- [Research sources and limits](docs/planned-features/SOURCES.md)
- [Open research/implementation gates](docs/planned-features/OPEN_QUESTIONS.md)
- [Implementation plan](tasks/FEATURE_IMPLEMENTATION_PLAN.md)
- [Specification review](docs/planned-features/SPEC_REVIEW.md)

## Continue development

Read [the consolidation handoff](docs/engineering/CONSOLIDATION_HANDOFF.md), [checkpoint](docs/CHECKPOINT.md), and [traceability](docs/TRACEABILITY.md) for current code and verification limits. The [integration review](docs/INTEGRATION_REVIEW.md) records branch reconciliation and safety findings.

Use the [engine research/specification prompt](docs/prompts/ENGINE_SPECIFICATION_PROMPT.md), now expanded to include the feature and UX package. Close the physical-model evidence gaps before claiming validated hardware emulation.

The current build line targets **Windows x64 VST3, CLAP and Standalone**, for both CircuitDrift and CircuitDrift LITE. AU and AAX remain outside this release line. See [the build decision](docs/adr/ADR-0001-build-formats-and-ids.md) for exact dependency pins, license and permanent product identifiers.

Validate the documentation package with:
```sh
python3 tools/validate_planned_features.py
```

That command checks specification consistency and example data. It does not build or test a plugin.

## Build and test

The portable core requires a C++20 compiler, CMake 3.22 or newer, and Python 3 for
registry/document checks. It does not fetch JUCE or require a graphics stack.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCD_SANITIZE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

The plugin build requires CMake 3.25 or newer and fetches the exact JUCE and CLAP
commits from `cmake/Dependencies.cmake`. On Windows, use the static MSVC runtime:

```sh
cmake -S . -B build-plugin -A x64 -DCD_BUILD_PLUGIN=ON \
  -DCMAKE_MSVC_RUNTIME_LIBRARY='MultiThreaded$<$<CONFIG:Debug>:Debug>'
cmake --build build-plugin --config Release --parallel 2
ctest --test-dir build-plugin -C Release --output-on-failure
```

Commands above use shell quoting; the repository workflows contain the tested Git
Bash form. Linux plugin development also needs JUCE's native audio/graphics
libraries; it is not a qualified distribution target.

## Source layout

| Area | Purpose and current boundary |
| --- | --- |
| `src/core`, `src/dsp`, `src/engine` | Parameter registry, numerical primitives and synthetic stereo A/B circuit engine |
| `src/control`, `src/runtime` | Musical modulation and host audio/MIDI/parameter integration |
| `src/state` | Documents, configuration history, preset library and parameter queues; complete dynamic host-session capture remains open |
| `src/blueprint` | Recovered graph/compiler, lifecycle and fault primitives; custom-graph installation into the newer engine remains open |
| `src/media`, `src/network`, `src/community` | Service primitives and tests; production provider, native MP4 and real host IPC qualification remain open |
| `plugin` | Full/LITE adapters, generic live editor, custom UI source and headless UI snapshot tests |
| `docs/audits`, `docs/engineering` | Source-pinned audit results, original branch inventory and fresh-worker handoffs |

No screenshot is presented as a finished live custom editor: its current snapshots
exercise `FakeBridge`, and the real `EditorBridge` still needs implementation.

## License

CircuitDrift is licensed under the GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later); see [LICENSE](LICENSE). Plugin binaries link JUCE 8 under its AGPLv3 option. Third-party components and their licences are listed in THIRD-PARTY-NOTICES.md.
