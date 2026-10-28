# Planned feature register

Every row is **Planned**. Source numbers are retained to avoid losing or renaming requested scope. Full means the full CircuitDrift edition. Stages sequence implementation; no stage removes a requested feature.

| ID | Requested feature | Scope / relationship | Engineering specification | Gate |
|---|---|---|---|---|
| CD-F03 | Visual GIF/Video Exporter | PNG, silent GIF, MP4 with optional plugin-output audio; local sharing files | [Interface/export](06-interface-inspector-export.md) | T03 |
| CD-F04 | Community Preset & Circuit Sharing | Cloud browse, publish, immutable versions, compatibility, offline cache | [State/community](05-state-presets-community.md) | T04 |
| CD-F05 | Free CircuitDrift LITE | Free two-knob Sag / Thermal Age edition and explicit upgrade | [LITE](07-lite-edition.md) | T05 |
| CD-F06 | One-Knob Drift & Aging Macros | Drift, Age, Warmth with defined composition and automation | [Musical controls](02-musical-controls.md) | T06 |
| CD-F07 | Resizable HD Vector GUI | 75–200% scale, dark/light themes and responsive layout | [Interface/export](06-interface-inspector-export.md) | T07 |
| CD-F08 | Tooltip Guidance & Educational Mode | Component-aware explanations, keyboard access, source provenance | [Interface/export](06-interface-inspector-export.md) | T08 |
| CD-F09 | Undo/Redo History Stack | 20 completed transactions with bounded memory and host interaction | [State/community](05-state-presets-community.md) | T09 |
| CD-F10 | Custom Circuit Blueprint Editor | Typed pin rewiring, component swaps, validation and compiled preview | [Blueprints](03-blueprints-timeline-faults.md) | T10 |
| CD-F11 | Multi-Bus Cross-Talk Network (IPC) | Shared supply/control state and separately enabled audio bleed | [Instance network](04-instance-network.md) | T11 |
| CD-F12 | Key-Tracked & Scale-Aware Thermal Drift | MIDI/manual root or monophonic pitch tracking; quantized resonance modulation | [Musical controls](02-musical-controls.md) | T12 |
| CD-F13 | Tempo-Synced Circuit Breathing | Tempo-based recovery and creative modulation, distinct from heat physics | [Musical controls](02-musical-controls.md) | T13 |
| CD-F14 | Harmonic Balance Targeting | Calibrated bias/drive lookup toward even/odd emphasis | [Musical controls](02-musical-controls.md) | T14 |
| CD-F15 | Dynamic Transient Recovery | Causal attack detector softens effective sag; retains physical reservoir state | [Musical controls](02-musical-controls.md) | T15 |
| CD-F16 | External Sidechain Power Starvation | Expands original sidechain: calibrated load, missing-bus fallback, linking | [Musical controls](02-musical-controls.md) | T16 |
| CD-F17 | 3D Thermal & Voltage PCB Inspector | Expands original inspector: read-only state, accessible 2D fallback | [Interface/export](06-interface-inspector-export.md) | T17 |
| CD-F18 | Interactive Circuit Degradation Timeline | Fresh/gigged/stored/restored anchors; reversible condition interpolation | [Blueprints](03-blueprints-timeline-faults.md) | T18 |
| CD-F19 | Component-Level Fault Injector | Bounded capacitor/resistor/tube faults with recovery and output containment | [Blueprints](03-blueprints-timeline-faults.md) | T19 |
| CD-F20 | Real-Time Thermal & Component Simulation Engine | Expands original engine: sample clocks, coupled thermal/supply state and numerical contract | [Runtime](01-runtime-and-quality.md) | T20 |
| CD-F21 | Internal Oversampling up to 16x | 1/2/4/8/16x; FIR and IIR options; latency and alias validation | [Runtime](01-runtime-and-quality.md) | T21 |
| CD-F22 | DC Offset Protection & Auto-Filtering | Switchable 10–20 Hz output HPF, default enabled, separate emergency containment | [Runtime](01-runtime-and-quality.md) | T22 |
| CD-F23 | Gain Staging Utilities | Input/output gain and measured level-matched parallel blend | [Runtime](01-runtime-and-quality.md) | T23 |
| CD-F24 | A/B State Comparison & Preset Management | Expands original profiler: independent states, switching/crossfade, import/export | [State/community](05-state-presets-community.md) | T24 |
| CD-F25 | CPU / Quality Scaler | Eco/Studio/Render with explicit rates, cost targets and no hidden downgrade | [Runtime](01-runtime-and-quality.md) | T25 |

LITE includes the underlying runtime, DC protection, fixed quality, local settings, scale/theme and two visible sound controls; its supported feature subset is defined in its spec. Full-edition features remain planned even where not included in LITE.
