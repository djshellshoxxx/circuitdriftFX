# CircuitDrift — research and engine specification prompt

Copy everything below the divider into the coding/research agent that has access to this repository. This is a prompt for producing the specifications, research record, and implementation plan; it is not a claim that the engine has been specified, implemented, or validated.

---

You are the lead analog-circuit researcher, audio DSP architect, and real-time plugin engineer for **CircuitDrift** in **djshellshoxxx/circuitdriftFX**.

Your assignment is to research and write the complete, internally consistent, engineer- and developer-ready specification package for the CircuitDrift engine. Save the actual deliverables in this repository, validate their consistency, and commit/push them through the available authorized repository workflow. Do not stop at an outline, a chat response, a bibliography, or a list of suggestions. Do not implement or release the production plugin during this specification task. Small reproducible research scripts, reference simulations, fixtures, and schema validators are in scope.

## 1. Mission, authority, and execution

1. Inspect the repository, its current branch and history, all applicable repository instructions, existing build files, and existing documentation before editing. Read **docs/PRODUCT_VISION.md** in full. Preserve that author-supplied vision; document corrections and interpretations separately. Do not overwrite unrelated work or force-push.
2. Also read docs/UX_DESIGN_BRIEF.md and the complete docs/planned-features/ package, especially FEATURE_REGISTER.md, DECISIONS.md, parameters.json, ACCEPTANCE.md and OPEN_QUESTIONS.md. All 23 CD-F03–CD-F25 features and UX-01–UX-10 contracts are mandatory planned scope. Preserve their stable IDs, reviewed source limitations and explicit design decisions; research and close their physical-model gaps rather than replacing them with generic outlines. Integrate this package into the final engine specs, add traceability, and reconcile any newly justified changes in an ADR. Do not claim planned features are already implemented.
3. Extract every feature, control, enum choice, numeric range, and behavioral claim into a requirements register. Give each requirement a stable ID. Separate the desired user experience from any unverified explanation of its physics.
4. First establish a capability map with stable module IDs, provider-owned interface contracts, and a dependency/build order. Acyclic software dependencies do not eliminate physical feedback: explicitly represent electrical, thermal, and supply loops in the solver design.
5. Research authoritative sources before selecting models. Compare viable approaches, select a coherent baseline, and record why it fits the target behavior, accuracy, stability, and CPU budget.
6. Work autonomously on reversible documentation, research, and validation. Choose reasonable provisional engineering values where the vision is silent, label them as design decisions, and explain their rationale. Do not quietly reduce scope or treat an assumption as the author's approved requirement.
7. Carry every mandatory feature through to a specification and verification method. Implementation phases may sequence features; they must not silently remove the inspector, A/B profiling, sidechain, MIDI, or any component family.
8. Keep a concise progress log and checkpoint the documents as work proceeds. If context is running short, write a handoff with completed files, unresolved decisions, source IDs, commands, and next actions, then continue from that state.
9. Separate an unavailable measurement from an unanswered design decision. When evidence is insufficient, specify the experiment or data needed, its blocking impact, and a bounded provisional model if defensible. Never mark an unresolved physical model as verified.
10. The updated author brief requires C++/JUCE and planned VST3, AU v2, AAX Native and CLAP targets. Specify Windows x64 and macOS Intel/Apple Silicon where each format applies; AU is macOS-only. Pin JUCE 8, validate the CLAP adapter and document AAX SDK/signing prerequisites. The later UX contracts define normal >=4x FIR plus explicit Off/2x Draft exceptions, zero-latency feature exclusions, ±24 dB utilities and 10–250 Hz HPF. This specification task does not require building binaries.

Success means another engineer can implement the engine and its tests without inventing its topology, numerical methods, units, parameter mappings, state semantics, event timing, failure behavior, or accuracy targets.

## 2. Research and evidence rules

Use live research tools and inspect the actual sources. Prefer original component datasheets and characteristic curves, manufacturer application notes and aging/reliability reports, published circuit schematics with clear provenance, peer-reviewed circuit/DSP papers, and official plugin SDK/framework documentation. Blogs and forum posts may identify leads; they are not the sole authority for core physical equations.

For each material claim, equation, coefficient set, curve fit, or algorithm, record:

- Stable source ID, title, author/manufacturer, year/revision, URL/DOI, access date, and exact page, section, figure, table, or equation where available.
- The component, circuit topology, operating conditions, units, and temperature/frequency/voltage range to which the source applies.
- Whether the value is **sourced**, **derived**, **measured**, **fitted**, **proposed**, or **unresolved**. These are distinct evidence states.
- A short paraphrase of the relevant evidence; distinguish your inference from the source's finding.
- Confidence, uncertainty, contradictory evidence, extrapolation limits, and any measurement/calibration needed.
- Source-code/model/data licensing and redistribution constraints. Public access does not establish reuse rights.

Verify equations and constants against primary sources. For central or disputed mechanisms, seek independent corroboration; do not pad the bibliography with irrelevant references. Resolve contradictory sources by comparing operating conditions and definitions. Do not invent a citation, a measurement, a datasheet value, a benchmark, a listening result, or a claim that a source was read.

Research must cover the actual model families under consideration: electrothermal networks; resistor temperature coefficients and noise; film/electrolytic capacitance, ESR, leakage and dielectric absorption; 12AX7 and EF86 characteristics; selected BJT/JFET devices; 5U4G, selenium and silicon rectification; supply storage and transformer impedance; magnetic hysteresis and parasitics; vibration-to-circuit coupling; nonlinear discretization and antialiasing; and current official VST3/host lifecycle constraints. For BJT/JFET and generic materials, select explicit representative devices/material grades instead of assuming a category has one universal set of coefficients.

If browsing or needed sources are unavailable, state that limitation precisely, finish unaffected work, and mark affected requirements evidence-blocked. Research search snippets alone do not qualify as inspected evidence.

## 3. Required repository deliverables

Use the following layout unless existing repository conventions require equivalent locations. Maintain an index mapping any changed paths. Write substantive content, not empty templates.

| Path | Required contents |
|---|---|
| docs/specs/README.md | Reading order, module map, spec status, glossary, notation/units, and package readiness |
| docs/specs/00-requirements-and-scope.md | Complete vision-to-requirement mapping, use cases, scope boundaries, assumptions, supported configurations |
| docs/specs/01-system-architecture.md | Signal, power, thermal, modulation and telemetry graphs; module ownership; clocks; complete processing order |
| docs/specs/02-reference-circuits-and-calibration.md | Reference boards, schematics/netlists, component values, operating points, loads, headroom and digital/physical calibration |
| docs/specs/03-component-models.md | Resistor, capacitor, inductor, tube, BJT/JFET, rectifier and transformer constitutive models and fitted data |
| docs/specs/04-thermal-network.md | Heat generation, transfer, dissipation, initial conditions and electrothermal coupling |
| docs/specs/05-power-supply-and-sidechain.md | Mains/rectifier/reservoir/regulation model, rail loading, recovery, bias interaction and external sidechain |
| docs/specs/06-aging-tolerances-and-dielectrics.md | Aging profiles, tolerance distribution/correlation, dielectric memory, leakage and reproducible variation |
| docs/specs/07-nonlinear-stages-and-magnetics.md | Active-stage topology/bias, transformer/core behavior, hysteresis state and circuit interaction |
| docs/specs/08-noise-hum-and-microphonics.md | Noise sources, spectral scaling, grounding/ripple paths, vibration and correlation |
| docs/specs/09-solvers-oversampling-and-stability.md | Discretization, convergence, antialiasing, multirate coupling, state bounds and numerical failure handling |
| docs/specs/10-parameters-automation-and-midi.md | Complete host/control contract, parameter mapping, automation, MIDI and modulation rules |
| docs/specs/11-ab-profiles-state-and-presets.md | A/B engine semantics, crossfades, state persistence, seeds, resets, migration and presets |
| docs/specs/12-audio-io-mix-and-protection.md | Bus layout, gain staging, dry/wet alignment, bypass, level matching and protection filters |
| docs/specs/13-engine-api-and-telemetry.md | Concrete interfaces, ownership/threading, lifecycle and 3D inspector data contract |
| docs/specs/14-performance-platforms-and-build.md | CPU/memory/latency budgets, platform/format matrix, dependency/version policy and build/CI design |
| docs/specs/15-validation-and-acceptance.md | Numeric acceptance criteria, reference comparisons, fixtures, commands and release gates |
| docs/specs/requirements-traceability.csv | Requirement -> source -> model/spec section -> parameter/API -> test -> implementation task -> status |
| docs/specs/parameters.json | Canonical, complete machine-readable parameter registry |
| docs/specs/schemas/ | Versioned JSON schemas for parameters, component/topology configuration, persistent state, presets, telemetry and test cases |
| docs/specs/examples/ | Schema-valid example configurations, A/B profiles, state/preset documents and telemetry messages |
| docs/research/SOURCES.md | Annotated evidence register with stable source IDs and exact locators |
| docs/research/claims-audit.md | Vision claims classified as supported, conditional, creative, contradicted or unresolved, with evidence |
| docs/research/model-comparison.md | Candidate model comparison, selected approaches, accuracy/CPU risks and rejected alternatives |
| docs/research/calibration-and-data-gaps.md | Fit/calibration procedures, uncertainty, missing data and reproducible measurement plans |
| docs/research/experiments/ | Reproducible reference scripts/netlists and result manifests when run; clearly distinguish plans from executed experiments |
| docs/decisions/ | Numbered architecture decision records for consequential choices |
| tasks/plan.md | Dependency-ordered implementation milestones and verification gates |
| tasks/todo.md | Bounded implementation tasks with requirement IDs, target files and acceptance/verification steps |
| docs/specs/OPEN_QUESTIONS.md | Unresolved decisions, impact, recommended default, closure method and blocking status |
| docs/specs/SPEC_REVIEW.md | Cross-document audit, checks actually run, failures, limitations and readiness verdict |

Update the root README with a concise project introduction and links to the vision, prompt, spec index, research, and implementation plan. Do not present planned features as implemented.

## 4. Standard for every engine specification

Each module must define, compactly and concretely:

1. Stable module ID, purpose, covered requirement IDs, boundaries, dependencies, and owned interface contracts.
2. Selected physical/reference topology and its supported domain; what is an approximation or intentional creative extension.
3. Equations with variable definitions, SI units, sign conventions, numerical coefficient tables, provenance, and a dimension check.
4. State vector, units, initial values, valid limits, channel/instance sharing, evolution rate, and persistence/reset policy.
5. Continuous-time model plus implementable discrete-time update equations or equivalent precise algorithm; not just a named paper or algorithm.
6. Inputs/outputs, ordering, parameter and modulation application, feedback/coupling, block-size behavior and host-event handling.
7. Pseudocode and proposed concrete typed interfaces sufficient to code the algorithm, including workspace needs and bounded loops.
8. Parameter ranges/defaults/mappings, transition behavior, calibration, and handling outside the physical model's valid range.
9. Numerical stability, convergence, error/fallback behavior, denormal/NaN/Inf handling, and real-time resource limits.
10. Specific tests with stimulus, reference, metric, numeric threshold, rationale, and expected failure interpretation.
11. Sources and validity limits; unresolved issues and decision status, explicitly distinguished from completed work.

Shared contracts may be defined once and linked. Do not duplicate constants across documents without identifying the canonical source. Units, symbols, defaults, IDs and state policies must agree throughout.

## 5. Mandatory physical and DSP content

### 5.1 Reference circuits and signal calibration

Select defensible reference schematics or explicitly labeled composite designs for the supported circuit personalities. The list of vintage consoles, preamps, tape electronics, passive EQs and early compressors describes a broad ambition, not a defined reference circuit. Decide which representative topologies deliver it and document the limits of each emulation.

Provide actual node/component definitions, nominal component values, supplies, source/load impedances, coupling/bypass networks, quiescent operating points, feedback paths and expected headroom. Explain what changes electrically when switching active element, rectifier, resistor or core type. Do not simply place incompatible device categories into the same circuit without defining adapters, topology changes and recalibration.

Specify the conversion between normalized floating-point audio, dBFS, dBu/dBV and physical volts, including peak versus RMS conventions and the reference impedance only where applicable. Derive dissipated power and rail current from the chosen circuit or a calibrated reduced model. Define control-voltage units separately from audio amplitude. Include startup/DC solution, supply warm-up, and a practical initialized state for immediate playback.

### 5.2 Resistors, capacitors and component interaction

For resistors, research temperature dependence, initial tolerances, voltage dependence where relevant, aging, thermal noise, composition-dependent excess noise, and parasitics needed by the selected topology. Distinguish resistance changes from noise generation and justify omitted effects.

For film and electrolytic capacitors, specify capacitance, ESR, ESL where relevant, leakage conductance/current, voltage/temperature dependencies, dielectric absorption branches, initial charge and energy storage. Give actual state equations and fitting procedures. Explain how leakage affects bias within the circuit and how that differs from adding an arbitrary DC offset at the output. Separate absorption, leakage and ESR rather than using one knob as an unexplained surrogate for all three.

Define how the component matrix propagates state changes through shared nodes and impedances. Quantify any reduction from full circuit solving to macro-models. Do not replace interaction with unrelated random parameter motion while describing it as coupled physics.

### 5.3 Thermal network and memory

Define thermal nodes and links, heat capacity, thermal resistance/conductance, ambient boundary conditions, and heat-source terms for each relevant component or component group. Research idle/heater/quiescent power as well as signal-dependent dissipation. Establish whether and how strongly audio at the chosen calibration changes each component's temperature.

Give the coupled energy balance, time constants, discrete integration, inter-component conduction, cooling, and any modeled temperature dependence. Define temperature limits, physical/proposed exaggeration ranges, drift signs, operating-point updates and runaway prevention. The model must reach an explained equilibrium and cool toward the appropriate baseline; thermal memory must not depend accidentally on host buffer size.

Separate physically plausible thermal dynamics from intentionally accelerated or magnified creative modes. Define state evolution during silence, transport stop, bypass, offline render, suspended processing and sample-rate changes without introducing wall-clock nondeterminism into the audio path.

### 5.4 Power supply, rectification, sag and sidechain

Specify mains RMS voltage versus internal DC rails, nominal transformer ratio, frequency, winding/source impedance, rectifier characteristics, reservoir charging/discharging, ESR, load, ripple, rail bounds and recovery. Distinguish instantaneous waveform simulation from a calibrated envelope model. Compare 5U4G, a sourced selenium implementation and a specified silicon diode configuration; provide operating-point-dependent differences rather than unexplained attack/release presets.

Explain how 90–130 V mains and rail capacity affect each supported topology. Define current draw by main-path stages, idle current, shared/stereo coupling, and intentional sidechain loading. For the sidechain specify accepted mono/stereo layouts, detection/filtering, calibration into current, sensitivity, attack/release or physical reservoir dynamics, channel linking, routing, missing-bus fallback and whether main and external loads add or replace one another.

Show how rail voltage changes headroom, bias and gain within the model. Audit the claim that sag necessarily produces pitch instability: identify and validate an actual phase/frequency-modulation mechanism if used, or classify it as a separately defined creative extension. Avoid inserting an undocumented pitch effect to satisfy the wording.

### 5.5 Aging and manufacturing tolerance

Define initial manufacturing tolerance separately from elapsed component age, operating history, thermal drift and random noise. Specify distributions, truncation, correlation between neighboring components and channels, seeds, and stable component identities. Apply the vision's up-to ±25% drift envelope as an explicit design bound requiring per-component interpretation; do not assume all components age linearly by the same percentage.

Research aging dependencies, validity domains and uncertainty for each affected family. Define how the 0–60-year control maps to component parameters when service history is unknown. Do not claim that a year count uniquely determines condition. Explain whether accelerated aging progresses during playback or whether age selects a static condition profile; distinguish that from ordinary thermal evolution.

Define repeatable L/R mismatch, mono compatibility, how changes are smoothed, and how new instances, presets, undo, project reloads and renders preserve the intended identity. Explain historical preset labels without implying a measured replica of unspecified equipment.

### 5.6 Tubes, transistors, bias and magnetics

For 12AX7 and EF86 stages, select cited device equations or curve-fitted models and specify load lines, bias networks, relevant electrodes, grid current/conduction behavior, feedback, supply dependence and capacitances where needed. For BJT and JFET stages, choose representative devices and specify the relevant current/voltage regions, junction/charge effects where needed, bias and temperature dependencies.

Provide coefficient tables, fitting domains and residual targets. Define the response to bias/drive automation and active-element switches, state transfer, recalibration and click prevention. Explain any reduced model's departure from the reference circuit; a waveshaper name alone is insufficient.

For inductors and transformers, specify winding ratio, magnetizing and leakage inductance, winding loss, core geometry/material basis, nonlinear flux behavior, hysteresis/remanence if modeled, DC bias, and capacitive parasitics. Compare suitable hysteresis/energy-based models and select an implementable one with its state equations and fit procedure. Separate saturation from hysteresis.

Resolve what “Nickel,” “Steel,” and “Iron” mean in terms of actual material/geometry or explicitly proposed presets; do not assign universal constants to those labels. Prevent double-counting overlapping transformer, inductor and output-stage coloration. Define flux/state initialization and recovery after silence or extreme input.

### 5.7 Noise, hum and microphonics

For each modeled noise source, specify physical origin, insertion point, spectral density, bandwidth, temperature/bias dependence, signal-path shaping, stereo/shared correlation and independent random stream. Derive the conversion from physical noise power to discrete-time variance and final dBFS. Document PSD conventions, equivalent noise bandwidth, oversampling/decimation effects and sample-rate invariance.

Distinguish Johnson–Nyquist noise from composition-dependent excess noise and any deliberately added artistic noise. Define whether shot/flicker noise is included or omitted and why. Specify PRNG algorithm, seed hierarchy, sequence advancement and reproducibility across block sizes; state the precision/tolerance expected across architectures.

Define mains pickup, supply ripple and harmonics as distinct paths with documented frequencies/phases and gain staging. Research whether each should scale with temperature instead of blindly applying one multiplier to all noise. Treat 50/60 Hz as selectable frequencies; audit regional shorthand rather than relying on it as a complete geographic rule.

Model microphonics with a defensible vibration/resonance-to-circuit coupling or explicitly labeled approximation. Specify excitation, resonant frequencies, damping/Q, feedback and stability bounds. If pitch wobble is included, define its mechanism and measurable limits separately from amplitude ringing.

### 5.8 Numerical solver, oversampling and stability

Compare applicable modified nodal analysis/state-space approaches, implicit integration, wave digital filters, reduced circuit models and hybrids. Evaluate methods against the actual feedback topology and state timescales. Select the main strategy and explain why alternatives were rejected.

Specify algebraic-loop handling, Jacobian/derivative calculation, nonlinear iteration, convergence norm and tolerances, maximum iterations, initial guesses, damping/step control and deterministic bounded fallback. Address discontinuities and singular/ill-conditioned cases. Give executable-quality pseudocode for one complete sample/substep and block, including the coupling order between voltage, current, thermal, aging, noise and modulation.

Select audio/electrical/thermal/control update rates and interpolation policies. Define discretization error and multirate coupling error budgets. State which changes require coefficient rebuilds and where those rebuilds occur. Define passivity/energy checks where applicable and distinguish powered energy injection from numerical instability.

Choose quality tiers and oversampling factors using evidence or explicitly proposed performance targets. Specify interpolation/decimation filters, orders, stopband/transition targets, nonlinear-stage placement, latency and CPU cost. Evaluate antialiasing methods against stateful nonlinearities; do not assume a method suitable for a memoryless curve automatically applies to the full circuit.

Do not treat the optional output low-pass as a substitute for preventing nonlinear aliasing. Cover parameter modulation aliasing, coefficient jumps, denormals, precision choices, overflow, NaN/Inf, extreme drive and silent-input limit cycles. Quality changes must have explicit transition, state and latency rules.

### 5.9 Audio I/O, master controls and protection

Specify bus layouts, stereo topology, channel sharing, in-place processing, input/output trims, drive location, ordering and headroom. Set the mix law, latency-aligned dry/wet path, dry endpoint behavior, phase implications and interactions with gain matching and filters.

Define level matching's measurement window/metric, gain limits, gating, response, silence behavior, stereo linking, noise interaction and interaction with host automation. Decide whether matching is a measured calibration feature or continuously adapting control, and justify it.

Specify high-pass and low-pass type/order, ranges, default/bypass states, DC behavior and state transitions. Distinguish optional sonic filters from mandatory finite-output/numerical containment. Do not promise DC rejection while permitting every DC-control path to be disabled without defining the resulting contract.

Define host bypass versus internal bypass, click-free transitions, delayed dry path, processing/tail behavior and any temperature/supply evolution while bypassed. Explain what “100% dry” guarantees and how global trims or mandatory protection affect that guarantee.

## 6. Mandatory control and host contracts

### 6.1 Complete parameter registry

Use parameters.json as the canonical registry, with a validating schema. Each parameter needs a permanent string ID and proposed stable host ID, display name, group, type/unit, physical min/max, default, step or continuous resolution, enum values, normalized-to-physical mapping and inverse, formatting/parsing, automation capability, smoothing domain/time, update rate, affected nodes, modulation policy, state version, validation rules and requirement/test IDs.

Preserve every author-specified choice and range:

| Group | Mandatory controls |
|---|---|
| Supply | Mains voltage 90–130 V; rail capacity in µF; rectifier: Tube (5U4G), Selenium, Silicon Diode; sag sensitivity |
| Condition | Component age 0–60 years; tolerance variance in percent; capacitor leakage; resistor: Carbon Comp, Metal Film, Wirewound |
| Thermal | Ambient 10–50 °C; thermal mass; drift coefficient |
| Topology | Gain/drive; active element: Triode Tube (12AX7), Pentode Tube (EF86), BJT Transistor, JFET; bias point; transformer: Nickel, Steel, Iron |
| Noise | Mains frequency 50/60 Hz; separate hum level; thermal-noise level; microphonics |
| Master | Separate input and output trim; automatic level matching; mix/blend; optional high-pass and low-pass controls |

Also define the controls required to operate A/B condition slots and their crossfade, sidechain power loading, MIDI routing/depth, quality, reproducible variation and inspector selection. Mark added controls as design proposals and explain why they are necessary. Do not create host parameters for every internal state variable.

Choose numeric defaults and missing ranges using the research and calibrated topology. Every invented range must be labeled a design decision. Separate physically calibrated operation from creative exaggeration. Explain how parameters with topology-dependent physical meanings remain stable for host automation, preset compatibility and UI display.

### 6.2 MIDI, modulation and automation

CircuitDrift is an audio effect receiving MIDI; do not silently turn it into a synthesizer. Define note-to-frequency and velocity mappings, reference pitch, target cutoff/bias/stress, modulation depth/limits, no-note behavior, channel selection, polyphonic note-selection policy, note-off/sustain/all-notes-off behavior and pitch bend support or exclusion.

Define event timestamps, within-block offsets, ordering of simultaneous automation/MIDI events, ramp interpolation, queue capacity, overflow policy, discontinuity handling and the exact point modulation enters the circuit. Account for host/format differences and unconnected MIDI buses. Resolve interactions among manual control, automation, MIDI, thermal drift and aging without hidden priority rules.

### 6.3 A/B profiling, state and reproducibility

Choose whether A/B uses two independently running circuits, parameter morphing, or another specified method. Compare sonic continuity, CPU cost, physically valid intermediate states and history preservation. Define which parameters are per-profile versus global, shared rail behavior, seed relationships, inactive-slot evolution, profile capture/swap/copy, crossfade curve, correlated-signal behavior, latency alignment, endpoint guarantees and switching under automation.

Provide a state inventory separating persistent user configuration, component identity/tolerance seeds, dynamic electrical/thermal/magnetic/dielectric memory, random-generator state, UI state and derived caches. Explicitly decide what survives project save/load, preset load, reset, duplication, sample-rate change, quality change and transport seek.

Use a versioned schema with backward migration, validation, size limits, unknown-field handling and corrupt-state recovery. Define exact reproducibility within one architecture/configuration and numerical tolerances across supported architectures. Explain how long-lived evolving behavior remains repeatable for offline rendering; do not interpret “unpredictable” as permission for uncontrolled nondeterminism.

### 6.4 Engine API, plugin lifecycle and inspector

Specify a framework-independent core boundary where practical, while selecting the proposed language/framework/dependencies in an ADR based on repository needs, current official support and licensing. Include concrete signatures and data structures for prepare/configure, reset, process, event submission, state capture/restore and telemetry consumption. Define ownership, lifetimes, maximum block/bus/event capacities, scratch storage, return/error types and thread affinity.

The audio callback must have explicitly bounded work. Prohibit heap allocation/deallocation, blocking locks/waits, file/network access, logging I/O and unbounded loops there. Define nonblocking handoff, snapshot consistency, queue overflow, topology rebuilds, worker preparation and safe reclamation. No GUI or GPU availability may be required for audio processing.

Specify actual VST3 process/lifecycle behavior using official sources: variable and zero-length blocks, precision modes, bus negotiation, activation/deactivation, sample-rate changes, event/parameter delivery, latency reporting, bypass, tail behavior, silence flags and offline processing. Separate format-required behavior from framework convenience behavior.

For the interactive 3D thermal/circuit inspector, define the engine-facing contract: stable board/component/node IDs, topology and optional layout coordinates, signal/rail voltages, temperature, current, power and relevant diagnostic values, each with units, timestamps and aggregation rules. Specify telemetry cadence, bandwidth, snapshot lifetime, decimation, heatmap range definitions, selection/probing and overload behavior. Report simulated quantities as simulated quantities; do not fabricate sensor precision or a physically exact PCB layout.

Specify a rendering/UI integration contract and proposed renderer boundary, but keep the specification task focused on engine data and behavior. Verify that closing or slowing the inspector does not change the audio result.

## 7. Performance, portability and build planning

Set explicit, justified target sample rates, variable block sizes, processing precision, channel layouts, maximum component/node/state counts, MIDI/automation capacity and quality tiers. Include small-buffer and high-sample-rate stress cases. Give CPU time in both milliseconds per callback and fraction of the callback deadline for specified reference hardware and settings.

Budget the complete engine, including both A/B paths, oversampling, noise, sidechain, state transitions and telemetry. State maximum iteration/work counts, memory, stack and latency budgets. Distinguish proposed budgets from measured results. Do not claim a CPU percentage without hardware, compiler/build, workload and measurement method.

Specify x64/ARM64 portability, SIMD/scalar fallbacks, floating-point differences, sample-rate normalization, deterministic seeding and thread assumptions. Research current framework/SDK versions, licenses and target requirements; record exact selected versions or a bounded version policy with rationale.

Provide the proposed source/test/build layout, formatting/error-handling conventions and full build/test commands for the eventual implementation. Commands for tools or targets that do not yet exist must be labeled proposed. Describe CI coverage, dependency pinning, validator/host tests and Windows/macOS artifact packaging. Separate compilation, host validation and signing/notarization requirements; do not claim any was performed during this documentation task.

## 8. Validation and numerical acceptance

Every mandatory requirement must have at least one uniquely identified acceptance test. For every test specify configuration, stimulus, duration/warm-up, random seed, sample rate/block size, reference source or independent oracle, measurement procedure, numeric pass/fail limit, rationale and evidence status.

Do not use “sounds analog,” “low CPU,” “stable,” “no audible artifacts,” or “small error” as the only criterion. Define numerical engineering targets even when no measurements yet exist, clearly marking those limits proposed. Do not claim a proposed tolerance is sourced or a planned test has passed.

At minimum, cover:

- Dimensional analysis, quiescent/DC operating points, transfer curves, small-signal frequency/phase response, impedance and headroom against sourced data or reference simulation.
- Component heating/cooling curves, idle versus driven temperature, rail droop/recovery, reservoir ripple and cross-component interactions.
- Harmonic spectra, THD and IMD versus frequency/level/bias; transformer hysteresis/minor-loop or equivalent model checks, remanence and low-frequency saturation.
- Dielectric charging/discharging/absorption, leakage-induced bias behavior, aging bounds, tolerance statistics/correlation, stereo divergence and mono fold-down.
- Noise PSD/integrated level and sample-rate invariance; hum/ripple frequencies and correlation; microphonic decay and bounded resonance.
- Alias energy over specified sweeps and modulation cases, with an independently higher-accuracy reference, justified FFT/window/analysis bandwidth and threshold.
- Mix/bypass endpoints, dry/wet alignment, declared latency, protection-filter responses, DC containment policy, level-matching accuracy and silence behavior.
- A/B endpoint continuity and profile independence, topology/quality switches, maximum parameter changes and sample-accurate event timing.
- Sidechain absent/present/mono/stereo cases, load calibration and its effect on the main circuit; MIDI note/chord/no-note transitions and queue pressure.
- Seeded repeatability, state round trips and migration, independent instances, offline versus real-time renders, transport/reload behavior and block-partition equivalence within declared tolerances.
- Long-duration silence/high-drive runs, extreme parameter combinations, NaN/Inf input, denormals, solver nonconvergence and bounded recovery.
- End-to-end worst-case CPU/memory/deadline behavior, multiple instances, GUI open/closed/stalled, host lifecycle and supported-platform validation.

Use appropriate independent reference models, published measurements, analytic cases and convergence studies. A SPICE model is a reference with assumptions and provenance, not automatic physical truth. Avoid circular tests that merely reproduce the production equations and call matching output validation. For listening tests, specify matched loudness, blinding, stimuli and decision criteria; listening complements quantitative tests.

Where a model can be checked now with a small script/reference simulation, do so and save the script, input provenance, tool/version, exact command, seed, results and uncertainty. Record missing dependencies or data honestly. Do not generate a mock “pass” table for unimplemented code.

## 9. Claim audit and architectural decisions

Audit every causal claim in the vision. Pay particular attention to:

- Whether realistic signal power can produce the asserted thermal drift on the relevant timescale.
- Whether rail sag alone justifies pitch instability.
- Whether age has a universal relationship to capacitance, resistance, noise or a ±25% shift.
- Whether leakage, dielectric absorption and phase changes are being conflated.
- Whether core material labels alone predict a transformer's response.
- Whether all noise/hum/microphonic sources share a temperature scaling law.
- Whether passive linear phase divergence, asymmetric nonlinear distortion, and perceived stereo widening are being distinguished.
- Whether the result is a defined hardware emulation, a composite virtual circuit, or an exaggerated creative effect.

Preserve creative intent while correcting unsupported physical interpretations. For each audit item give the evidence, classification, engineering consequence and proposed user-facing description. Do not claim calibrated physical fidelity or exact historical equipment replication without the evidence to support it.

Create ADRs for reference topologies, calibration, solver/discretization, multirate coupling, oversampling, shared rails, aging distribution, deterministic variation, A/B design, persistence policy, parameter IDs, framework/format support and telemetry. Each ADR needs context, alternatives, selected/proposed decision, evidence, consequences and reversal cost.

## 10. Implementation plan and final quality gate

Turn the specifications into a dependency-ordered plan: reference/calibration tooling; core types and component models; solver and coupled circuit; controls/state/events; plugin integration; inspector contract; performance/validation. Adjust that order where evidence requires it. Each task must identify requirement/spec/ADR IDs, affected files, prerequisites, concrete outputs, acceptance checks and whether it is blocked. Give tasks that can be implemented and reviewed in a focused session.

Before committing the package:

1. Verify every original feature, enum and range is represented and traceable. No mandatory feature may disappear behind an “MVP” label.
2. Validate JSON and schema examples, ID uniqueness, parameter enum/default/range consistency, internal links, traceability references and state migrations/examples. Add a small validator if needed; run it and record the result.
3. Audit units, calibration, signs, timebases, coupling order, sample-rate scaling, global/profile ownership, parameter priority and reset behavior across documents.
4. Check that equations resolve to specified coefficients or explicitly blocked calibration work; identify physical versus proposed artistic behavior.
5. Check that every real-time loop and queue has a bound and failure policy, and that every required behavior has a quantitative acceptance test.
6. Review source provenance/licensing and distinguish executed experiments, proposed targets and missing evidence.
7. Record all actual checks and remaining blockers in SPEC_REVIEW.md. Use a truthful verdict: implementation-ready, ready only for named bounded modules, or blocked for named reasons. Documentation completeness does not prove the plugin works.
8. Commit and push the completed documentation/research changes using repository conventions and existing authorization. Verify the remote files/commit. If permission or connectivity blocks publishing, preserve the completed work and report the exact blocker; never claim remote success without verification.

Finish with a concise report linking the spec index, research register, implementation plan and commit/PR. State the selected core modeling approach, readiness verdict, checks actually run and any unresolved blocking decisions. The deliverable is the saved, researched specification package, not merely this report.

Begin by inspecting the repository and reading docs/PRODUCT_VISION.md, then perform the research and write the complete package.
