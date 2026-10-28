# CircuitDrift planned feature specifications

Status: **planned; no plugin implementation exists in this repository yet.** Revision 1, 2026-10-28.

This package specifies all 23 additions/expansions in the author's numbered list (items 3–25; item 2 was blank). It supplements the original [product vision](../PRODUCT_VISION.md). The [feature register](FEATURE_REGISTER.md) preserves the requested scope and maps each feature to an engineering contract and acceptance test.

**Normative language:** MUST is required for implementation acceptance; SHOULD is a justified preference. All product defaults, capacities and numeric acceptance limits here are proposed engineering decisions, not hardware measurements or already-achieved performance. Research-backed constraints are identified by source IDs in [SOURCES.md](SOURCES.md). Device coefficients and actual reference circuits still need the original engine research assignment; see [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md). This package does not falsely certify those missing physical models.

## Read in this order

1. [Runtime, quality, protection and gain](01-runtime-and-quality.md): common clock, processing contracts and baseline engine.
2. [Macros and musical modulation](02-musical-controls.md): macro composition, scale, tempo, harmonic targeting, transient recovery and sidechain.
3. [Blueprints, timeline and faults](03-blueprints-timeline-faults.md): bounded graph compiler, history-dependent conditions and failure simulations.
4. [Multi-instance network](04-instance-network.md): shared PSU, cross-talk and offline limitations.
5. [State, history and community](05-state-presets-community.md): A/B, presets, undo and cloud service.
6. [Interface, inspector and export](06-interface-inspector-export.md): responsive GUI, education, telemetry and recording.
7. [LITE edition](07-lite-edition.md): two-knob product, compatibility and upgrade.
8. [Acceptance matrix](ACCEPTANCE.md), [parameter registry](parameters.json), [implementation plan](../../tasks/FEATURE_IMPLEMENTATION_PLAN.md).

9. [UX design and analytical displays](08-ux-interaction-and-displays.md), [author UX brief](../UX_DESIGN_BRIEF.md), and [resolved design decisions](DECISIONS.md). UX-01 through UX-10 extend the original feature IDs.

## Capability boundaries

| Module | Owns | Depends on |
|---|---|---|
| runtime | clocks, solver execution, audio buses, quality, protection | validated compiled models |
| control | macros, musical and transient modulation | runtime control contract |
| blueprints | typed graph, compiler, fault/timeline transforms | model registry and runtime configuration contract |
| state | presets, A/B slots, history, state migration | control and blueprint serialized contracts |
| network | member discovery, shared demand/audio transport | runtime network ports |
| community | catalog, accounts, revisions, local cache | state and blueprint import validators |
| inspector | read-only telemetry and interaction | runtime telemetry, blueprint identities |
| exporter | capture and encode local media | inspector frames, optional output-audio tap |
| lite | edition-specific controls and fixed graph | runtime, control and state |

Build in this dependency order; physical feedback inside runtime is explicitly solved and is not represented as a software dependency cycle. Cloud, network discovery, graph compilation and media encoding never execute on the audio callback.

## Decisions applying to every feature

- Keep all original functionality; duplicate requests strengthen existing requirements rather than create a second competing system.
- Requested formats are VST3, AU, AAX and CLAP. Windows x64 and macOS x64/arm64 are target architectures; AU is macOS-only. Use C++20/JUCE 8 with a pinned release and a validated CLAP adapter. AAX SDK/signing and host qualification are explicit release dependencies. No binaries exist yet.
- UI controls changing audible behavior are either permanent host parameters or explicit non-automatable transactions. Dynamic blueprints cannot regenerate host parameter IDs.
- Normalized controls use [0,1]. Smoothing is per sample using a=exp(-1/(Fs*tau)); tau=0 means exact event boundary. Discrete actions follow the owning specification.
- Physical modeling, creative modulation and fault experimentation are separately identified in the UI and preset metadata.
- Feature IDs CD-F03 through CD-F25 are stable. Corresponding tests are T03 through T25; additional invariants are in ACCEPTANCE.md.
- Existing original-engine parameters are not replaced by the extension registry. New permanent host IDs occupy 1000–1999; existing/base-engine IDs must use a disjoint range. Published IDs cannot later be reassigned.

Run the package's implemented documentation checks with: `python3 tools/validate_planned_features.py`. Runtime acceptance tests are specified, not implemented or passed.
