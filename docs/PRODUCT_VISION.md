## Overview: CircuitDrift

**CircuitDrift** is a real-time analog physics and component-degradation effect plugin (Audio FX). Unlike traditional analog modeling plugins that rely on static transfer curves, fixed EQ profiles, or standard hysteresis loops, CircuitDrift simulates an active virtual circuit board operating under dynamic physical constraints.

Every virtual component—resistors, film and electrolytic capacitors, inductors, vacuum tubes, discrete transistors, and power transformers—possesses dynamic values that fluctuate based on operating temperature, current draw, component age, and supply voltage. As signal flows through the plugin, internal components heat up, power rails sag under heavy low-end transients, and aged dielectric materials alter their capacitance, delivering an evolving, organic, and truly unpredictable analog sound.

## What It Emulates & Physical Behaviors

CircuitDrift models the micro-behaviors of vintage discrete analog audio hardware (vintage console channel strips, tube preamps, tape machine electronics, passive EQs, and early solid-state compressors).
```scss
┌──────────────────────────────────────────────────────────────────────────┐
│                             CIRCUITDRIFT ENGINE                          │
│                                                                          │
│  [ INPUT AUDIO ]                                                         │
│        │                                                                 │
│        ▼                                                                 │
│  ┌───────────┐    Thermal Model     ┌─────────────────────────────────┐  │
│  │ Input     │ ───────────────────► │ Component Value Drift           │  │
│  │ Stage     │                      │ (Resistors / Capacitors)        │  │
│  └─────┬─────┘                      └────────────────┬────────────────┘  │
│        │                                             │                   │
│        ▼                                             ▼                   │
│  ┌───────────┐    Voltage Draw      ┌─────────────────────────────────┐  │
│  │ Saturation│ ───────────────────► │ Power Rail Sag & Bias Drift     │  │
│  │ Engine    │                      │ (Rectifier / Tube Loading)      │  │
│  └─────┬─────┘                      └────────────────┬────────────────┘  │
│        │                                             │                   │
│        ▼                                             ▼                   │
│  ┌───────────┐    Aged Dielectrics  ┌─────────────────────────────────┐  │
│  │ Filtering │ ───────────────────► │ Dynamic DC Offset & Phase Shift │  │
│  └─────┬─────┘                      └─────────────────────────────────┘  │
│        │                                                                 │
│        ▼                                                                 │
│  [ OUTPUT AUDIO ]                                                        │
└──────────────────────────────────────────────────────────────────────────┘

```

### Key Emulation Mechanics

- **Thermal Memory & Dissipation:** Virtual components heat up as current passes through them. Higher signal levels accelerate thermal buildup, altering resistor resistance and drifting filter cutoffs, tube bias points, and saturation thresholds upward or downward over time.
- **Power Supply Sag & Rail Droop:** Simulates unregulated linear power supplies and vacuum tube rectifiers. Heavy bass transients draw significant current from the virtual power rail, dropping internal DC voltage. This results in soft dynamic compression, subtle pitch instability, and transient rounding.
- **Dielectric Absorption & Capacitor Leakage:** Old electrolytic capacitors leak small amounts of DC current and exhibit "memory" effects, altering low-frequency response, introducing subtle phase smear, and creating asymmetric waveform distortion.
- **Inductor Hysteresis & Transformer Saturation:** Models core saturation, inter-winding capacitance, and non-linear magnetic hysteresis in custom audio transformers, resulting in low-end harmonic bloom and high-frequency smoothing.
- **Ground-Plane Interference & Noise Floor:** Models Johnson–Nyquist thermal noise, 50Hz/60Hz mains hum, and microphonic tube jitter, which scale realistically with internal circuit temperature and gain staging.

## Core Features & System Architecture

- **Dynamic Component Matrix:** A real-time node network where changes in one component’s thermal state or voltage level ripple through adjacent circuit paths.
- **Interactive 3D Thermal & Circuit Inspector:** Real-time visual representation of the virtual PCB showing component temperatures, voltage heatmaps, and dynamic signal flow.
- **A/B Circuit Aging Profiler:** Allows users to set up two different circuit conditions (e.g., "Factory Mint 1968" vs. "Unserviced Studio Gear 1978") and crossfade seamlessly between them.
- **Sidechain Power Loading:** Route an external track (e.g., a kick drum or bassline) into the sidechain to intentionally starve or overload the main audio path's power supply rails.
- **MIDI Tracked Filter & Bias:** Sync virtual circuit stresses, tube bias shifts, or filter cutoffs directly to incoming MIDI note pitch and velocity values.

## Control Layout & Parameters

### 1. Power Supply & Voltage Stage

- **Mains Voltage (Volts):** Adjusts the virtual AC wall power feed ($90\text{V} - 130\text{V}$). Undervolting ("brownout") increases saturation, lowers headroom, and increases power sag.
- **Rail Capacity ($\mu\text{F}$):** Sets the size of the power supply filtering capacitors. Lower values yield heavy dynamic sag and pumping; higher values stabilize the circuit.
- **Rectifier Type:** Switch between **Tube (5U4G)**, **Selenium**, and **Silicon Diode** power supplies, altering the recovery speed and tightness of the voltage rails.
- **Sag Sensitivity:** Controls how severely low-frequency transients drain the internal DC power rails.

### 2. Component Condition & Aging

- **Component Age (Years):** Global aging factor ($0 - 60\text{ Years}$). Gradually drifts component tolerances (up to $\pm 25\\%$), degrades capacitor dielectric properties, and increases internal noise.
- **Tolerance Variance ($\\%$):** Introduces random value discrepancies between Left and Right stereo channels, creating natural analog stereo widening and phase divergence.
- **Capacitor Leakage:** Dial in DC offset leakage and low-end phase smearing caused by degrading capacitors.
- **Resistor Composition:** Select virtual resistor types (**Carbon Comp**, **Metal Film**, or **Wirewound**), changing the temperature coefficient and noise characteristics.

### 3. Thermal Dynamics

- **Ambient Temp ($^\circ\text{C}$):** Sets the baseline ambient temperature of the virtual studio enclosure ($10^\circ\text{C} - 50^\circ\text{C}$).
- **Thermal Mass:** Controls how quickly virtual components heat up and cool down in response to incoming audio signal levels.
- **Drift Coefficient:** Magnifies or dampens the degree to which thermal buildup shifts filter cutoff frequencies and operational bias points.

### 4. Circuit Topology & Saturation Stage

- **Gain / Drive:** Controls the signal level feeding into the initial amplification stage.
- **Active Element:** Toggle between **Triode Tube (12AX7)**, **Pentode Tube (EF86)**, **BJT Transistor**, and **JFET** gain stages.
- **Bias Point:** Adjusts the DC operating point of the active element. Off-center settings create pronounced asymmetric harmonic distortion (even-order harmonics).
- **Transformer Core:** Choose between **Nickel**, **Steel**, or **Iron** transformer cores to shape saturation saturation knees and low-end harmonic coloration.

### 5. Noise, Hum & Microphonics

- **Mains Hum Frequency:** Toggle between **50 Hz** (UK/EU) and **60 Hz** (US/JP) power line noise, with dedicated level control.
- **Thermal Noise (Johnson Noise):** Controls the level of continuous wideband thermal noise generated by resistors.
- **Microphonics:** Introduces physical vibration sensitivity, causing subtle pitch wobble or ringing when hit with loud transient spikes.

### 6. Master & Output Controls

- **Input / Output Trim:** Digital utility gain controls with automatic level matching option.
- **Mix / Blend:** Parallel processing control to blend the raw, pristine input signal with the degraded, drifting circuit output.
- **High-Pass / Low-Pass Protection Filters:** Optional clean digital filters to restrict extreme subsonic DC drift or excessive ultrasonic harmonic buildup.
