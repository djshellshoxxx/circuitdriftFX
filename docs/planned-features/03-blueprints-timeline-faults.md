# Blueprint editor, lifecycle and component faults

Planned contract: CD-F10/F18/F19, UX-01/UX-03. Tests T10/T18/T19. Uses [runtime](01-runtime-and-quality.md), [control](02-musical-controls.md), [state](05-state-presets-community.md). S06 motivates careful distinction between creative circuit changes and validated physical behavior.

## Blueprint model and compiler

Blueprints are declarative JSON, never executable scripts, plugins, shader source, downloaded SPICE directives or native libraries. A modelId selects a built-in, versioned, independently validated device manifest. Supported catalogue targets are resistor (composition variant), film/electrolytic capacitor, inductor, 12AX7, EF86, specified BJT/JFET, rectifier, transformer and defined input/output/supply ports. The actual device models remain a research gate; unsupported catalogue entries cannot be previewed as if implemented.

Stable node/component IDs survive moving a component in the view. Pin names/types and parameter units are supplied by the model manifest; the stored graph maps each pin to an electrical node. Geometric wire crossings do not imply connections. Model swaps show incompatible pins and require explicit remapping; never guess a tube pin assignment. Geometry affects display only unless a future explicit parasitic-layout model is selected.

Hard limits: 1 MiB UTF-8 uncompressed blueprint, nesting depth <=16, 64 components, 33 nodes including ground "0", 256 pin connections, 64 thermal nodes, 8 nonlinear devices and 96 electrical unknowns. IDs <=64 ASCII characters, names <=80 Unicode code points. Reject duplicate JSON keys, non-finite numbers, unknown executable fields, path/URL resource references, unknown schema major version and duplicate identities.

Workflow: Idle -> Draft -> Validating -> Ready or Invalid -> Preparing -> Applying -> Idle. Keep current audio active during edits. Validation/compiler worker returns diagnostics by component/pin and a candidate cost report. Apply only a successfully prepared immutable graph generation, at a callback boundary, with 20 ms crossfade to the old graph. No processing-thread compilation or matrix resize. Retire old memory after acknowledgment. A compile failure leaves the previous graph and saved state untouched.

Compiler checks in order:
1. JSON schema and resource bounds.
2. Model/version existence, pin completeness/type, parameter range/unit, nonzero positive thermal C and bounded conductances.
3. Electrical connectivity, ground reference and input/output terminals. Reject floating required pins/islands, inconsistent ideal voltage-source loops, unsupported capacitor/inductor/source constraints and accidental supply shorts. Finite leakage/gmin may be used only where the manifest declares it; do not silently repair user wiring.
4. Symbolic MNA structure/unknown count and numeric DC convergence within 100 off-thread Newton iterations.
5. A 250 ms synthetic probe at the active Fs and chosen quality with silence, -18 dBFS sine, and -6 dBFS transient. Require finite output, no emergency fallback and all model-domain bounds.
6. Prepare block storage, factor fixed thermal matrices, map stable states and bind macros. Mark result Validated for this configuration, not universally stable.

Preserve capacitor charge, magnetic and thermal state only for matching component ID+model revision and a manifest-supported parameter transition; other components initialize at prepared DC/thermal point. Do not inject old charge into a different topology without a defined transform. A/B double paths plus graph transition must respect a maximum of four prepared processors; coalesce pending Apply requests rather than stack unbounded fades.

```cpp
// Worker only; bounded input already parsed.
CompileResult compile(const Blueprint&, const ModelRegistry&, const RuntimeConfig&);
// Audio: consumes a preallocated handle, never owns its deallocation.
bool queuePreparedGraph(Generation, PreparedHandle) noexcept;
```

Expose 16 permanent Mod Slots (0..1) for custom component controls. Binding a slot to a component/field is a non-automatable transaction stored in the blueprint; it does not create/change host IDs. Changing a binding while automation is active requires Apply and a clear target-name update. Direct gestures use an existing base parameter or assigned slot; unbound custom fields are transaction-only controls.

## Lifecycle timeline

A scalar Hardware Lifecycle position u in [0,1] interpolates condition, not elapsed running time. Default scenario labels are 1975 Factory Spec, 1990 Tour-Worn, 2010 Attic Storage, 2026 Derelict. Alternate supplied 1972/1985/2005/2026 labels and original 1975/1985/2005/2026 Restored are available scenario metadata; dates do not select unique physics.

Each scenario stores four explicit condition anchors and positions {0,1/3,2/3,1}. Each interval uses smoothstep h(t)=t*t*(3-2*t). Positive physical values interpolate in log space; signed bias offsets interpolate linearly; discrete topology does not morph. Values are applied as offsets/multipliers over base parameters and before macros. The display year is linearly interpolated between label years; a reversible scrub does not accumulate extra damage.

Proposed creative anchor table (dimensionless multipliers except noted):
| Condition | Age years | C multiplier | ESR multiplier | Leakage fraction of safe span | Tolerance sigma | Bias offset/span | Noise addition dB | Drift multiplier |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Factory | 0 | 1 | 1 | 0 | 0.0033 | 0 | 0 | 1 |
| Tour | 15 | 0.95 | 1.4 | 0.01 | 0.02 | 0.015 | 3 | 1.5 |
| Storage | 35 | 0.8 | 3 | 0.08 | 0.05 | 0.04 | 9 | 3 |
| Derelict | 51 | 0.65 | 6 | 0.2 | 0.08 | 0.08 | 15 | 5 |
| Restored ending | 5 | 1 | 1 | 0 | 0.0033 | 0 | 0 | 1 |

Per-component manifests clamp these creative multipliers to validated ranges. The fresh sigma corresponds to approximately ±1% at three sigma, with explicit truncation. Age noise is still gated by Analog Noise; no surprise noise addition when disabled. "Zero leakage" means the intentional creative leakage term is zero; the underlying reference device may still have physical leakage.

Restored is a scenario ending with replacement-component condition, not decreasing chronology or a claim of universal repair success. At the last anchor, stable component IDs persist while a replacement-generation counter changes identity seeds only when Apply Restoration is committed. Continuous preview interpolates condition without reseeding. One scrub gesture is one Undo transaction; seed changes occur once on commit.

Automation of u is sample timestamped, smoothed 50 ms; the visual scrubber follows the host. The actual seconds-long thermal state does not instantly jump to a target temperature. Show both condition target and live temperature. Existing state/history is preserved when switching Easy/Workbench.

## Fault injector

Faults are per-component overlays with type, severity 0..1, enabled flag, PRNG seed and revision. Default all off. The user explicitly chooses a component and fault, previews impact, then Apply; enabling is reversible and part of Undo/A/B/preset state. A fault is an artistic bounded failure simulation unless a validated failure model exists.

| Fault | Parameter action | Transition / limit |
|---|---|---|
| Electrolytic open | reduce C to manifest minimum and raise series resistance toward its declared open equivalent | interpolate log values over 10 ms; never ideal infinity |
| Electrolytic leaky/short | lower parallel leakage resistance toward manifest short-equivalent bound | no ideal zero resistance; current/domain limiter remains |
| Cracked carbon resistor | raise resistance up to declared open-equivalent; optional seeded intermittent contact | 2 ms smooth transitions, max 20 transitions/s |
| Microphonic tube | excite damped modal bias perturbation from main transient detector | f=800 Hz default, 100..4000 Hz; Q 2..30; bias <=5% safe span |

For a microphonic mode, z[n]=2*r*cos(2*pi*f/Fs)*z[n-1]-r^2*z[n-2]+g*excitation, r=exp(-pi*f/(Q*Fs)); normalize g by measured peak impulse response during preparation, bound injection and states to the bias-span limit. It runs at electrical Fs*OS. This is a proposed resonant mechanical-coupling surrogate, not a measured tube response. "Wild" fault descriptions never authorize unbounded digital output.

Continuous severity is assignable to a stable mod slot; fault type is non-automatable. When any active fault would require an unvalidated topology it stays unavailable with a reason. Emergency containment and dry fallback cannot be disabled by a fault or imported blueprint. Reset Faults clears overlays with a 20 ms ramp but does not erase genuine thermal state or the rest of the preset.

T10 rejects dangling pins, invalid loops, excessive resources, unsupported models and malformed import without altering current audio. T18 checks exact anchor targets, reversible scrubs, replacement seed semantics and 20-step undo. T19 sweeps every supported fault at 0/0.5/1, silent and -6 dBFS driven, requiring finite bounded output and reproducible seeded intermittent patterns.
