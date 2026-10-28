# CircuitDrift LITE

Planned contract: CD-F05; T05. LITE is a separate free plugin product with permanent, distinct plugin identifiers in every supported format. It shares the validated engine code; it is not a timed trial and requires no cloud account or recurring network check to process audio.

## Product surface

Exactly two primary sound knobs: **Power Supply Sag** and **Thermal Age**, normalized 0..1, defaults 0.25 and 0.2. Settings menu may contain scale/theme, bypass/reset, accessibility, version/help and an explicit "Explore full CircuitDrift" link. No hidden third tone knob. Fixed mix=100%, automatic matching enabled, input/output trims=0 dB, 15 Hz HPF and soft-clip protection enabled. Fixed clean factory topology; no faults, graph editing, IPC, community publishing, A/B dual engines or media recorder.

Sag uses the full Sag macro. Thermal Age drives Age=a and Drift=0.25*a with creative pitch-delay disabled; physical thermal sensitivity remains. Warmth fixed 0.2, Analog Noise off. All mapping constants are versioned as liteMappingVersion=1. Free does not imply lower numerical stability: default normal processing is 4x FIR Eco with the same finite-output policy. The full UI and 3D workbench are absent; a simplified sphere/rail meter is read-only.

LITE is supported on the same requested format/platform targets once each adapter is qualified. Do not silently promise an AAX binary before SDK/signing gates close. Stable format IDs must never collide with full product IDs or change with licensing.

## State and upgrade

Store schemaVersion, edition="lite", liteMappingVersion, sag, thermalAge, seed, quality revision and UI settings. Save/reload preserves deterministic configuration. Upgrade export creates a full scene with fixed graph/base controls and exact mapped macro values, seed and model version; the full edition imports it without changing its audible behavior. Full presets requiring unsupported features are rejected by LITE with a compatibility summary, never silently reduced.

No automatic host plugin replacement or purchase on clicking upgrade; open the product page only on deliberate user action. LITE continues to work after installing the full edition. No modal marketing during playback, no expiry noise or mandatory signup. Cloud browsing/commerce infrastructure is not a dependency of free audio operation.

## Verification

T05 renders identical seeded input through LITE and the equivalent full configuration and requires <=1e-6 peak difference after latency alignment. Test offline cold launch, preset reload, zero knob endpoints, high sample rates and explicit upgrade export. Confirm exactly two primary sound controls, no full-only parameter leakage and distinct plugin IDs. Full/LITE build targets compile the same engine library with edition manifests; no fork of core numerical algorithms.
