# Handoff (2026-10-29)

Read first: CLAUDE.md → this file → docs/CHECKPOINT.md → docs/TRACEABILITY.md → tasks/FEATURE_IMPLEMENTATION_PLAN.md.

## State
- main = squash 29ca74b (PR #1 merged): CMake scaffold, generated parameter registry (74 params), ThermalNetwork, ButterworthHpf, softClip, GainMatcher, acceptance tests, Linux CI (green on GitHub, Linux only).
- Product is NOT complete. Done: P01 partial, P03 primitives. Everything else Planned (see TRACEABILITY.md). No JUCE, UI, formats, binaries, solver, oversampling, state, or physical validation.
- Session note: the Claude session started in the unrelated repo `CLASSifiedisRUPTed`; work was done in a clone of `djshellshoxxx/circuitdriftFX` attached via add_repo. The designated branch name `claude/circuitdriftfx-implementation-dknarc` exists in both repos; only circuitdriftFX carries work.

## Verify the baseline
```sh
cmake -S . -B build -DCD_SANITIZE=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure
```
Expected: 3/3 pass (acceptance_core, registry_codegen_fresh, docs_validation_only).

## Next tasks (in order)
1. P02/P03: device-manifest schema (q, f, Jacobians, domain bounds, provenance) + MNA/Newton solver (scaled partial-pivot LU, caps 6/8/12, damped trials, failure fallback) in `src/dsp`; reference test against an independent closed-form/ODE solution using a labelled SYNTHETIC RC + diode fixture (G01 stays open).
2. T21: polyphase FIR oversampling (≥100 dB stopband, ≤0.01 dB ripple), latency padding, 64x reference comparison.
3. Finish T22/T23 integration: gain/mix 10 ms smoothing, enable crossfades, per-channel stateful wrapper, OS-stage soft clip, emergency containment (non-finite→0, ±8 clamp, 10 ms dry fade).
4. P04 state: versioned JSON, transaction queue, checkpoints. Then P05+ per plan.
5. Add real-time alloc/lock instrumentation test early (UX-09) and block-partition determinism test ({1,17,64,127,512,8192}).

## Decisions/assumptions recorded
- Soft clip "bounded below 1" tested as ≤1 in double (tanh saturates ≥|x|≈2.8); strict <1 checked up to 2.5.
- Thermal prepare rejects islands without an ambient path explicitly (backward Euler alone would not).

## Blockers needing owner
G01 device/reference schematics; G09 JUCE 8 licence (AGPLv3 vs commercial) and CLAP adapter pin; G08 AAX SDK/signing; G06 cloud provider; G07 media encoder SDKs; Windows/macOS toolchains and real hosts for format qualification. No spend, accounts, publishing or uploads have been done.

## Caching/context
Prompt caching is a client mechanism; none configured or measured here. Keep CLAUDE.md stable; progress goes in CHECKPOINT.md/this file.
