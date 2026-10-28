# UX interaction and analytical display contracts

Planned implementation specification for the [author UX brief](../UX_DESIGN_BRIEF.md). Read together with [interface/telemetry/export](06-interface-inspector-export.md) and [decisions](DECISIONS.md). These contracts resolve repeated or conflicting descriptions without discarding either creative or analytical intent.

## UX-01 — PCB direct manipulation

Workbench uses orbit/pan/zoom camera controls on background; a component hit selects that component. Single-clicking a component never flips the view. Workbench/Pop Hood button (or clicking the Easy sphere's explicit Open Workbench affordance) opens the advanced view. A 180-degree board-flip transition lasts 240 ms, purely visual; Reduce Motion replaces it with a 100 ms fade. No audio state change occurs.

Hit regions project component bounds and expand to at least 32 logical pixels; overlap resolution chooses nearest visible depth, with a cycling picker for stacked objects. Selection identity is component UUID, not render-object index. Pointer capture persists until up/cancel; Escape restores pre-gesture values. Pointer loss commits last valid value and ends host gestures, avoiding stuck automation.

| Target / active tool | Gesture | Mapping and transaction |
|---|---|---|
| Tube / Bias | vertical drag | normalized bias delta=-dyLogical/200; clamp 0..1; Shift fine scale 0.1 |
| Capacitor bank or transformer / Supply | pinch or horizontal stretch | C_new=C_start*r^2 where r=pinch-distance ratio; Sag macro contribution delta=-log2(r)/4; clamp to manifest and 0..1 |
| Resistor cluster / Age brush | path drag | add signed accumulated pathLength/400 to selected components' condition-age fraction and linked excess-noise modifier |
| Resistor cluster / Material | click/context selector | explicit Carbon Comp, Metal Film, Wirewound choice; apply once, no interpolation between enum labels |
| Any editable value | double-click numeric field | parse units/range, show error without changing state |
| Selected editable component | arrows / Shift arrows | ±1% / ±0.1% normalized adjustment; Home resets selected control's factory base value |

dyLogical is screen delta divided by UI scale once. Pinch only begins after two pointers are registered; desktop equivalent is drag handles or mouse wheel over the selected Supply tool. Pinch multiplier r is clamped 0.5..2 per gesture, avoiding device jumps. "Transformer capacity" is a supply-capacitor macro handle, not a claim a transformer stores capacitance in microfarads.

Age brush default increases condition; Alt reverses. Brush preview highlights affected IDs, freezes that set at pointer-down, and never automatically selects newly crossed unrelated components. Noise linkage affects composition-dependent excess/creative noise; Johnson noise still follows the physical model. Material is a separate tool so the same gesture never ambiguously changes Age and composition. Tooltip states which parameters will change.

Every continuous gesture starts one host edit transaction, publishes bounded targets at <=120 Hz and ends on release; actual DSP smooths targets per the control spec. Multi-target edits are grouped where the wrapper supports it; core publishes one coherent internal generation. A gesture is one undo record. A custom field lacking a stable Mod Slot is visibly non-automatable and applied through the graph transaction, not a dynamically invented host ID. Keyboard and numeric controls can perform every audible action without 3D pointing.

## UX-02 — Easy versus Workbench

Easy header: product, style preset menu, A/B endpoints, Workbench. Center: Tone State Sphere. Lower row in canonical order Age, Sag, Drift, Warmth. Utility bar: Input, Auto-Gain, HPF enable/cutoff, Mix, Output, soft-clip status and HQ selector. Advanced disclosure may collapse nonessential labels at minimum size but cannot hide bypass/output.

Workbench header: Hardware Lifecycle, scenario, A/B, Return to Easy. Center: PCB and selectable layer toolbar. Right: component inspector drawer. Lower area: thermal scope, rail plot and phase plot tabs plus the same macro controls. Both views edit the same APVTS driver/base state. Theme, view, plot choices and camera are UI state only; resizing/switching cannot dirty a sonic preset.

Quick style presets are versioned creative factory scenes:
| Name | Age | Sag | Drift | Warmth | Noise | Intent |
|---|---:|---:|---:|---:|---|---|
| Clean Console | 0 | 0 | 0 | 0 | Off | calibrated clean base |
| Warm Tube Preamp | 0.15 | 0.2 | 0.1 | 0.55 | Off | moderate tube drive |
| Pumping Bus Glue | 0.1 | 0.8 | 0.1 | 0.35 | Off | sag-driven coloration |
| Lo-Fi Attic Gear | 0.85 | 0.65 | 0.6 | 0.45 | On | intentionally degraded |

Preset names do not imply exact emulation of unspecified historical hardware. Use declared factory graph/seed/model versions, 100% wet and Auto-Gain on; loading is one transaction. Workbench edits mark Modified. Rendering-only mode switches must produce bit-identical audio in a deterministic fixture.

## UX-03 — Hardware Lifecycle scrubber

Top bar labels the continuous value Hardware Lifecycle, with four scenario anchors and a current interpolated year/condition. Use the timeline contract, including selectable Derelict versus Restored endings. Canonical default labels are 1975/1990/2010/2026; preserve the alternate requested dates in scenario presets. Reversible preview never mutates a component's permanent age history. Mouse, wheel, keyboard and direct percentage entry work; one drag is one undo.

Show resulting condition properties and live temperature separately. Disable overhyped labels such as "perfect physical age measurement"; these are authored condition profiles. Future parameter migrations retain scenario ID/revision so changing factory anchors never changes old projects.

## UX-04 — Thermal scope and honest hysteresis labels

Modes:
1. **Transfer XY:** x=latency-aligned circuit-input volts, y=circuit-output volts; equal volts-per-pixel axis scaling by default. Compensation removes declared processing delay only, retaining the circuit's actual frequency-dependent phase. Mono/L/R selectable. A unity linear reference produces a 45-degree line only at equal gains/scales; display this in help.
2. **Voltage / Temperature:** x=input or selected-node volts, y=selected-component Celsius, with explicit separate units. No sum of volts and degrees.
3. **Voltage / Rail:** x=input volts, y=selected DC rail volts or normalized fraction, clearly labeled.
4. **Magnetic B-H:** available only when the selected transformer model supplies H [A/m] and B [T]. Otherwise disabled with "model does not expose magnetic state." An ordinary XY loop is not proof of magnetic hysteresis.

Process bounded paired samples on analysis worker. Downsample with antialiasing to <=48 kHz; reduce to <=2048 trace points/frame at 30 updates/s using ordered extrema-preserving decimation; do not sort points by amplitude. UI may animate at 60 FPS using existing geometry. 3D ribbon z-axis represents recent time (0–250 ms), not invented physical dimensions. Clear on graph/epoch change; stale data fades with a badge.

Trace color has selectable source Temperature or Stress. Temperature uses model-specific cool/warm/stress ranges; the supplied 20–35,45–65,80+ °C palette is available as an explicitly illustrative global range and cannot be described as an optimal operating range for every tube/resistor. Stress combines separately labeled rail droop/clipping/temperature-normalized indicators, with a numeric legend. Cyan/amber/red is never the sole indication of state.

## UX-05 — Power rail waveform

Display actual selected internal DC rail V(t) and its nominal V_nom, not AC wall RMS. AC Mains 90–130 V is a separate control/readout. A 120->92 V trace is permitted only when the active model actually has that nominal DC rail and telemetry shows that droop.

Analysis retains 10 seconds of 1 kHz min/mean/max rail telemetry. Default viewport last 2 s, adjustable 0.1–10 s; decimate by display column with min/max envelope. Plot absolute sample-derived time and show ms/beat grid. Cursor readout identifies nearest 1 ms bin and aggregation interval, not fictitious sample-precise voltage. Annotate minimum rail and measured time to recover 95% of droop relative to pre-event level, or "not recovered"; reject overlapping-event recovery measurements rather than report the wrong duration. Sync target and measured recovery are separate lines/labels.

Gain meters show pre/post sample peaks and RMS, hold peaks 1 s, clip indicator 2 s. Rail severity is not a loudness meter and cannot change gain.

## UX-06 — Stereo phase scatter

Analysis worker performs stereo STFT with 2048-sample Hann windows, hop 512 at the native or antialiased <=48 kHz analysis rate. For each positive-frequency bin, compute smoothed cross spectrum S_LR=E[X_L*conj(X_R)], powers S_LL/S_RR with 250 ms time constant, phase phi=arg(S_LR), coherence gamma2=|S_LR|^2/(S_LL*S_RR+epsilon), and channel balance b=(S_LL-S_RR)/(S_LL+S_RR+epsilon). Clamp gamma2 to [0,1] for numerical roundoff.

Plot polar radius=log(f/20)/log(fmax/20) clamped 0..1, angle=phi, opacity=gamma2, size=magnitude, color=channel balance. Labels and an alternate Cartesian frequency-versus-phase plot make the mapping readable. Hide bins below -70 dBFS or coherence<0.1. A UI particle corresponds to a real frequency bin, not an arbitrary particle simulation forced into twin orbits.

In-phase channels cluster near zero angle; a constant delay creates a frequency-dependent phase slope; unrelated channels fade through low coherence. Phase alone does not establish perceived width, and DC offset is not inferred from phase bins. Show separately the 500 ms mean pre-protection L/R voltage/DC difference and a fullband normalized correlation meter. Mono processing displays Not applicable. Artistic twin-orbit animation may exist only as an explicitly decorative alternate view, never in the analytical scatter.

## UX-07 — Tone State Sphere

Sphere is an expressive summary, labeled Tone State; it is not a calibrated oscilloscope. Drive it from shared snapshots with 80 ms visual smoothing:
- r_fraction=clamp(selected rail/V_nom,0,1); squash=0.25*(1-r_fraction).
- Base radius=0.85+0.10*clamp(RMS_dBFS+60,0,60)/60; vertical scale=1-squash, horizontal scale=1+0.5*squash.
- Color interpolation cyan->amber->crimson uses normalized measured nonlinear residual energy from a calibrated linear reference when available; otherwise use Warmth parameter with a "control preview" flag, not a false measured distortion claim.
- Edge displacement amplitude <=3% radius from 0.5*Age+0.5*Drift; deterministic render noise seeded separately from audio. Animate only while enabled; render RNG must never advance the DSP RNG.
- Silence holds a calm ring; Reduce Motion disables deformation/pulsing while retaining numeric Sag/Warmth indicators.

Sphere and PCB receive the same snapshot timestamp and agree on shared rail/temperature. Selection of a different visualization never changes sound or recorder timing.

## UX-08 — Utility behavior and DSP sketch corrections

Use input/output ±24 dB, mix 0..100%, HPF 10..250 Hz, default 15 Hz, output soft clip and explicit HQ Off/2/4/8/16. Auto-Gain initial implementation is linked RMS matching as specified in runtime; LUFS may be added as a separately validated meter/control mode, not mislabeled RMS as LUFS.

Charge integration requires dt and electrical units: dV/dt=(I_charge-I_load)/C. For a reduced linear charge path, V_new=(V_old+dt*(V_nom/(R*C)-I_load/C))/(1+dt/(R*C)); R in ohms, C in farads, I in amps. The supplied abs(audio) term needs calibrated conversion to current and is not by itself a physical load model. The production manifest supplies nonlinear rectifier/load behavior; the formula is a testable reduced case.

The supplied temperature expression lacks thermal state/time evolution and is replaced by the heat ODE. Creative bounded colored modulation is separate. Static tanh with bias has no memory; retain capacitor, thermal, magnetic states and documented feedback to justify memory/hysteresis. Minimum normal quality is 4x FIR; Off/2x are explicit Draft exceptions. Zero Latency disables every delay-producing feature, not merely FIR.

## UX-09 — Realtime implementation boundaries

Use APVTS for stored driver/base parameters and attachments, never perform ValueTree copies/mutations on audio. Effective micro-values live in a fixed DSP snapshot with no feedback writes to automated parameters. Parameter listeners must assume the caller can be realtime; they may only update lock-free values or queue bounded events. All graphics, shader compilation, preset JSON, history allocation and OS calls stay outside processBlock.

AbstractFifo supports a single writer/reader and owns indices, not payload storage (S13). Allocate payload first; do not use one queue as MPMC. Verify atomic lock-freedom and alignment on every target. Sample-accurate automation cannot be reconstructed from a single atomic final value: wrappers must pass timed events when supported and expose any format/adapter limitation.

## UX-10 — Acceptance and performance

Check all interactions at every supported scale/theme and minimum viewport; no clipped essential controls, mismatched hit targets, stuck gestures or inaccessible numeric alternative. Deterministic reference fixtures: unity XY diagonal error <=0.5 display pixel at fixed scale; known 0.5 gain slope correct within 1%; known stereo delay phase error <=3 degrees in coherent bins away from wrap; injected 50 ms rail recovery displayed within 2 ms; unit/nominal labels correct. Timestamps survive resize/capture; no stale frames cross graph generation.

Target 60 FPS, <120 MB idle and the explicit 1x CPU/zero-latency contracts in runtime. These are targets requiring measurements, not evidence that an implementation exists. Tests must include GPU loss, minimized editor, closed editor, 200% HiDPI, two open instances, active host automation, keyboard-only use and Reduce Motion.
