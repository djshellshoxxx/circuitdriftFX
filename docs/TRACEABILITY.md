# Traceability (requirement → implementation → test → evidence → status)

Status vocabulary: Planned / Partial (primitive only) / Implemented / Blocked(gate). Nothing is "Implemented" unless its full acceptance test passes against the integrated engine.

| ID | Implementation | Test | Evidence (2026-10-28, gcc+ASan/UBSan, x86-64 Linux) | Status |
|---|---|---|---|---|
| P01 registry | src/core/ParameterRegistry.*, tools/gen_param_registry.py | acceptance_core `registry`, registry_codegen_fresh | 74 params, unique IDs 1000–1215, round-trip/endpoints/NaN pass | Partial (no JUCE/format wrappers; G08/G09) |
| CD-F20 / T20 | src/dsp/ThermalNetwork | `t20_thermal` | synthetic fixture err ≤0.002 °C at 10 s; 2-node steady state exact; island/C/asymmetry rejected | Partial (thermal only; MNA solver + manifests open; G01/G03) |
| CD-F22 / T22 | dsp::ButterworthHpf | `t22_hpf` | −3.0103±0.05 dB at 15 Hz at 44.1/48/96/192 kHz; DC residual <−100 dBFS after 2 s | Partial (no smoothing/crossfade, bypass path, stateful per-channel wrapper) |
| CD-F23 / T23 | dsp::softClip, dsp::GainMatcher | `t23_clip_match` | knee C1, bounded ≤1 (see note), RMS match ±0.5 dB, freeze <−60 dBFS, clamp [0.25,4] | Partial (no gain/mix smoothing, OS-stage clip, draft-mode check) |
| Others (F03–F19, F21, F24, F25, UX-01–10) | — | — | — | Planned |

Note: spec says soft clip is "bounded below absolute 1"; in double precision tanh saturates to exactly 1.0 for |x|≳2.8. Mathematically strict, numerically ≤1. Reversible interpretation recorded; no threshold weakened.
Docs validator (`tools/validate_planned_features.py`) passes — documentation validation only.
