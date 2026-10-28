# CircuitDrift UX design brief — author requirements

Status: planned. Added 2026-10-28. This records the author's supplied UX descriptions with repeated passages consolidated. Illustrative diagrams are expressed below as layouts and interaction descriptions. Conflicting variants and proposed math remain visible; their engineering resolution is in [UX contracts](planned-features/08-ux-interaction-and-displays.md) and [decisions](planned-features/DECISIONS.md). This brief is design intent, not a measurement or validated physical model.

## Interactive Thermal Topology Grid / PCB Workbench

The main advanced view is an interactive 3D glowing circuit board, replacing a static faceplate of knobs with a virtual laboratory workbench. Users manipulate components directly:
- Drag a vacuum tube vertically to adjust DC Bias; its glow responds to temperature/stress.
- Pinch/stretch a power transformer or capacitor bank to adjust rail capacitance and sag.
- Scrape/drag across a resistor cluster to increase Component Age and Thermal Noise; another described gesture changes Carbon versus Metal Film composition.
- Toggle X-Ray/component layers, Thermal Camera, Voltage Heatmap and Signal Flow Vector Lines.
- Reveal component values, tolerance matrices, schematic wiring and custom mod slots when the board flips into Workbench mode.

Example content includes a 12AX7 tube labeled 78 °C, a 470 µF capacitor labeled Leakage and a connecting voltage-sag trace. These are illustrative readouts; live values must be supplied by the engine.

## Dual-view interface

**Overview / Easy Mode:** clean single-screen vector HUD, dark brushed-metal aesthetic with light-mode support, four controls labeled Age, Sag, Drift and Warmth, central Tone State Sphere, quick style presets, utilities and a Workbench button. Earlier descriptions use the simplified PCB as its centerpiece; the final Easy Mode description instead requests the sphere.

**Workbench Mode:** Pop Hood / Workbench reveals the interactive GPU-rendered 3D PCB, editable values, wiring, component details and analytical tools. A prominent Hardware Lifecycle scrubber and A/B controls sit above the board. Thermal oscilloscope, power-rail waveform and macro controls sit below it. The stereo phase scatter plot supplies an additional diagnostic view.

Switching modes must preserve all sound settings and avoid macro/micro automation conflicts.

## Hardware Lifecycle / Circuit Scrub timeline

The slider morphs component condition smoothly through evocative historical stages. The supplied descriptions contain three naming variants:
1. 1972 Factory Mint -> 1985 Tour-Tested -> 2005 Attic Storage -> 2026 Derelict/Failing.
2. 1975 Factory Spec -> 1990 Tour-Worn -> 2010 Attic Storage -> 2026 Failing/Derelict.
3. Earlier feature request: 1975 Factory Fresh -> 1985 Gigged Hard -> 2005 Attic Storage -> 2026 Fully Restored.

Fresh means tight tolerances (illustratively ±1%), clean recovery and no intentional DC leakage/noise. Touring adds mild thermal drift, harmonic coloration and sag. Storage adds capacitor deterioration, asymmetric bias and noise. Derelict emphasizes brownouts, microtonal movement and phase smear. Restoration is a distinct outcome, not synonymous with continued deterioration. Dates describe scenario anchors, not universal hardware aging laws.

## Four macro controls

- **Age:** pristine/modern at 0%; vintage/dusty at 100%, with rolled-off highs, optional noise, channel variation and capacitor wear.
- **Sag:** stiff rail/punchy transients at 0%; heavy rhythmic pumping, transient rounding and smooth compression at 100%.
- **Drift:** stable tuning at 0%; thermal/filter motion, subtle chorus and microtonal pitch movement at 100%.
- **Warmth:** clean at 0%; tube/transformer drive, low-end bloom and harmonic saturation at 100%.

These endpoint descriptions concern each macro's contribution; their relationship to independently edited Workbench controls must be defined. Style presets: Clean Console, Warm Tube Preamp, Pumping Bus Glue and Lo-Fi Attic Gear.

## Tone State Sphere

A single organic visualizer communicates tone without electronics jargon:
- Size/pulsing: audio and rail sag cause rhythmic expansion/contraction/squashing.
- Color: cool cyan for clean, amber/gold for warmth, deep crimson for heavy drive.
- Edge texture: smooth at clean/stable settings, increasingly rippled with Age/Drift.
- A top-right Workbench button opens the advanced view; Auto-Gain and style presets remain easy to reach.

## Analytical displays

**Thermal Oscilloscope / Hysteresis view:** input voltage versus output response, with thermal/rail-state color and optional 3D ribbon history. A linear unity-gain signal should draw a 45-degree line at equal axis scales. Nonlinear or phase-shifted signals may form curves/loops. The brief also describes voltage-versus-temperature/rail plots; distinct axis modes are needed rather than mixed units.

Proposed colors: cyan/blue 20–35 °C, amber 45–65 °C, red at 80 °C+. Descriptions call amber an optimal saturation region; model-specific validity must be established rather than assumed for all components.

**Power Rail Waveform:** nominal and actual supply voltage plotted against milliseconds, adjacent to gain metering. Example 120 V dropping to 92 V during a kick and recovering; nominal AC mains and internal DC rails must be distinguished. Recovery visualization helps tune tempo-related pumping.

**Phase Drift Scatter:** a particle per frequency bin showing the L/R phase relationship; channel mismatch and aging should visibly spread the cloud. Twin orbits are an artistic concept. The display also seeks to explain stereo widening, DC divergence and phase depth; exact measurable quantities require separate definitions.

## Utility bar

Input Trim -24..+24 dB; Auto-Gain toggle; High-Pass 10..250 Hz; Mix 0..100%; Output -24..+24 dB with soft-clipping protection; HQ Off/2x/4x/8x/16x. Auto-Gain should enable fair comparisons using RMS/LUFS-style level measurement. High-pass is post-processing and removes subsonic content/DC. Local preset JSON stores macro values and detailed component states with backward compatibility.

## Supplied DSP sketches to audit before implementation

The following equations express the author's intended behavior, not yet dimensionally complete implementation formulas:

V_rail[n] = V_rail[n-1] + (V_nominal-V_rail[n-1])/tau_charge - abs(x[n])/tau_discharge.

T_component[n] = T_ambient + alpha*RMS_audio[n] + beta*PinkNoise[n].

Requested intent: capacitor charge/discharge state driving nonlinear headroom and coupling-filter behavior; audio-dependent thermal mass plus smooth stochastic variation; delay variation Delta t <=1.5 ms; biased saturation with memory/hysteresis. The brief proposes modified tanh/polynomial nonlinearities and requires polyphase FIR antialiasing, normally at least 4x. Explicit Off/2x modes and a 1x benchmark are also requested. Correct continuous/discrete units, actual state memory, solver coupling and exceptional low-quality modes must be defined in the engineering spec.

## Implementation and performance requirements

- C++ with JUCE 7/8 family; pin one supported version, not two simultaneous frameworks.
- Planned formats VST3, AU, AAX and CLAP; native rates 44.1–192 kHz.
- Vector GUI 75–200%, Retina/HiDPI; hardware acceleration through appropriate OpenGL/Metal/Direct3D/Vulkan abstraction.
- Target 60 FPS for sphere/PCB with audio and GUI rendering decoupled.
- Target CPU <1.5% per instance in 1x/Easy mode on a specified modern Apple Silicon/x86_64 reference system.
- Zero reported samples in explicit zero-latency mode when every latency-producing stage is disabled.
- Memory <120 MB per instance, with recording-resource accounting specified separately and openly.
- No audio-thread heap allocation/deallocation, resizing containers, blocking mutexes, file/network access, logging I/O or graphics calls.
- Use bounded lock-free parameter/telemetry communication; verify atomics are lock-free on supported targets.
- Smooth audible parameter changes; report actual latency; maintain DC protection.
- Macros are normalized drivers of effective micro-parameters, with APVTS-facing state and no circular feedback.
- Do not push unbounded raw audio streams to the GUI; use bounded meter/scope/analysis bridges.
- Easy/Workbench are views of one sound state, not independently evolving presets.
