# Feature implementation plan

This is the full dependency plan. P01–P04 have partial primitives; consult [current traceability](../docs/TRACEABILITY.md) and the [continuation prompt](../docs/prompts/CONTINUE_DEVELOPMENT.md) for verified progress, validation repairs and the next bounded P03 integration slice. Read [spec index](../docs/planned-features/README.md), [acceptance](../docs/planned-features/ACCEPTANCE.md) and [gates](../docs/planned-features/OPEN_QUESTIONS.md).

| Stage/task | Deliverable and proposed source area | Dependencies / gate | Verify before advancing |
|---|---|---|---|
| P01 Contracts and build scaffold | C++20/JUCE 8/CMake pin, format identities, parameter registry generation; src/core, src/adapters, CMakeLists | G09; AAX conditional toolchain G08 | Load empty effect in each available wrapper; stable IDs/state; CI matrix identifies blocked formats |
| P02 Reference models | Device manifests, calibrated circuits, offline model harness; models, research, tests/reference | G01 | Independent curves/DC/thermal references, provenance and domain checks |
| P03 Runtime and protection | MNA/thermal/supply clocks, oversampling, gain/mix/HPF/clip; src/dsp | P02, G03 | T20–T23, real-time traps, impulse latency |
| P04 State foundations | Immutable configuration, versioned JSON, checkpoint mechanism, parameter transaction queue; src/state | P01/P03 | Schema rejection/migrations and coherent snapshots |
| P05 Macros/sidechain/transients | One-way macro evaluator and current-load detector; src/control | P03/P04, G02 | T06/T15/T16 and zero-effect endpoints |
| P06 Tempo/key/harmonics | Tempo clock, scale quantizer, pitch worker and calibrated harmonic LUT; src/control | P05, G01/G10 | T12–T14, confidence and fallback |
| P07 A/B/history/local presets | Dual continuous states, copy/swap, 20-step history and import/export; src/state | P04/P05 | T09/T24, worst-case history memory |
| P08 GUI shell/Easy mode | Responsive vectors, themes, utilities, sphere, factory scenes; src/ui | P05/P07 | T07/T08, UX-02/07/08/09, keyboard and resize |
| P09 Inspector/analysis | Stable telemetry, plots, GPU adapter and 2D fallback; src/ui/analysis, src/ui/pcb | P08, G11 | T17, UX-04/05/06/10; no audio coupling |
| P10 Blueprint compiler/editor | Typed manifests/graph validator, immutable compile/apply, Mod Slots; src/blueprint | P02/P04/P09 | T10 and UX-01; hostile input and graph bounds |
| P11 Timeline/faults | Condition scenarios, restoration identity, bounded fault overlays; src/blueprint | P10/P07, G02 | T18/T19 and UX-03 |
| P12 LITE target | Edition manifest, two-knob shell, exact full-scene export; src/edition | P03/P05/P08 | T05, offline use and identity |
| P13 Recorder | Read-only capture, native encoders, timestamps and local export; src/media | P09, G07 | T03, queue/disk failures and memory cap |
| P14 Community | Native auth, catalog client, backend API, cache/moderation; src/community, services/catalog | P04/P10, G06 | T04 including ownership, offline and malformed revisions |
| P15 IPC realtime | Local broker, prepared IPC rings, shared supply/current and bleed; src/network | P03/P04, G05 | T11 realtime failure matrix and explicit limitations |
| P16 IPC deterministic render | Co-render harness and hashed influence bundles/replay | P15 | T11 replay equivalence across callback orders |
| P17 Quality/performance | SIMD where justified, profiling, max-graph bounds and zero mode; src/dsp, benchmarks | P03–P16, G03/G04 | T25 and UX-10; no silent reduction of accuracy |
| P18 Host/release qualification | VST3/AU v2/AAX Native/CLAP builds, validators, signing and packaging | All; G08/G09 | Full acceptance matrix, platform host smoke tests, documented remaining exclusions |

Each stage should be broken into small reviewed changes with specific tests; run independent reference research and adapter preparation concurrently only when authorized tools/workflows support it. This plan does not authorize service purchases, external messages, account registration or release publication.

## Definition of done

All 23 feature IDs and 10 UX IDs trace to implemented behavior and passing checks, physical-model claims have independent evidence, requested format/architecture matrix is tested, runtime/UI resource budgets are measured, and release notes distinguish creative approximations from validated emulation. Until then the feature register remains Planned or explicitly In progress, never Implemented by virtue of having a spec.
