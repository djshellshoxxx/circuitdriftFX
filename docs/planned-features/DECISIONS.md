# Design decisions and resolved overlaps

These are proposed implementation decisions made from the author's two additions, not claims that the author individually approved each numeric default. They preserve the requested scope and make contradictory sketches implementable.

| Decision | Resolution / reason | Consequence |
|---|---|---|
| D01 App versus planning | Repository has no plugin implementation; add all features as Planned with contracts and tests | No claim of working DSP/UI/builds |
| D02 Scope | Original F03–F25 retained; UX-01–UX-10 add interaction detail | Duplicate sidechain/inspector/runtime requests strengthen one system |
| D03 Four macros | Age, Sag, Drift, Warmth; macros derive effective state without rewriting host base parameters | One-way APVTS ownership; no circular automation |
| D04 Views | Easy defaults to Sphere; Workbench uses 3D PCB; simplified PCB may be an optional Easy visual | Same audio state; no hidden mode reset |
| D05 Timeline | Default 1975/1990/2010/2026 Derelict; supplied alternative dates and Restored ending are scenario variants | Date labels do not pretend age uniquely specifies condition |
| D06 Resistor gestures | Age brush and Material selector are separate explicit tools | No ambiguous gesture changes both condition and composition |
| D07 Transformer gesture | Supply handle changes reservoir capacitance/sag, not the transformer's physical capacitance | UI indicates affected fields and units |
| D08 Math | Capacitor charge and heat ODEs include dt, units, state and calibrated load/power; sketches preserved in UX brief | Physical coefficients still require original research assignment |
| D09 Drift/pitch | Thermal drift and creative colored/filter/delay motion are distinct | Delay movement <=±1.5 ms; zero-latency and scale-lock behavior explicit |
| D10 Hysteresis | Static tanh is not memory; XY transfer is not automatically B-H hysteresis | B-H view only when magnetic H/B are actually modeled |
| D11 Oversampling | Normal Eco/Studio >=4x FIR, Render 16x; Off/2x labeled Draft; optional IIR explicitly low-latency | Preserves low-quality selector and 1x benchmark without mislabeling quality |
| D12 Latency | Zero mode disables OS, creative delay, lookahead and audio IPC bleed; changing mode uses host handshake | "FIR off" alone cannot guarantee zero samples |
| D13 Output | Latest utility range ±24 dB; HPF 10–250 Hz includes original 10–20 Hz protection region; soft clip separate from emergency guard | Default HPF 15 Hz; no claim of protection while bypassed |
| D14 Auto-Gain | Precisely specified linked RMS method initially; no unimplemented LUFS claim | Longer-term LUFS option requires independent validation |
| D15 Colors | Component-specific temperatures and measured rail stress; universal color bands only illustrative | Amber is not asserted a universal physically optimal temperature |
| D16 Phase/rail plots | DC rail separate from AC mains; phase/coherence derived from STFT; DC shown separately | No decorative orbit presented as scientific measurement |
| D17 IPC | Bounded approximate realtime coupling plus explicit deterministic co-render/replay path | Arbitrary host offline order does not magically synchronize |
| D18 Formats | C++20/JUCE 8; VST3/AU v2/AAX Native/CLAP; correct OS matrix | Pin dependencies, CLAP adapter and AAX signing before release |
| D19 CPU/memory | Define <1.5% as callback p99/deadline at explicit 1x reference; <120 MB idle; capture exception explicitly <184 MB | Targets unmeasured; don't report success from documentation |
| D20 Saving/publication | Declarative local presets; cloud upload requires explicit Publish; recorder writes local media only | Audio never depends on account/network availability |
| D21 Physical readiness | Software/UX contracts implementable; device manifests/calibration remain blocked | A complete wrapper must not be marketed as validated component physics |
