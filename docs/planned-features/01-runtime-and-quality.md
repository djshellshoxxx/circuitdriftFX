# Runtime, quality, protection and gain

Status: planned engineering contract. Covers CD-F20/F21/F22/F23/F25; T20/T21/T22/T23/T25. Dependencies: validated model manifests and original engine research. Sources: S01–S05, S09, S11 in [SOURCES](SOURCES.md). All numeric targets below are proposed.

## Execution and supported configurations

Use a C++20 framework-independent core; JUCE 8 is the selected wrapper/UI family; exact release and license must be pinned before implementation. Support mono->mono and stereo->stereo, optional mono/stereo auxiliary input, 32/64-bit host samples, 44.1/48/88.2/96/176.4/192 kHz and variable blocks of 0–8192 frames. Split larger supplied buffers into fixed prepared slices without allocation. A zero-frame call applies events/state only and advances no clock.

States use double precision in every quality mode. Quality affects oversampling and solver effort, not storage precision. A sample counter, retained across callback boundaries, drives all control clocks. No state is advanced by UI frames or wall time. Resume after suspend advances only when samples arrive; elapsed real time is not simulated. Transport seek starts a new timeline epoch and clears pitch, tempo phase-lock and network history; capacitor/thermal/magnetic state remains unless a Reset transaction is requested.

Audio-thread APIs (all noexcept, bounded and preallocated):

```cpp
struct ProcessContext { double sampleRate; int64_t sampleStart; uint64_t epoch;
  uint32_t frames; bool offline; Transport transport; };
ProcessResult process(const ProcessContext&, AudioBuses,
                      EventSpan, TelemetryWriter&) noexcept;
void reset(ResetKind) noexcept;
bool tryInstallPrepared(PreparedCircuitHandle) noexcept;
// Non-audio: compileBlueprint, prepare, migrateState, savePreset, encodeMedia.
```

Handle reclamation occurs on a worker after the audio consumer acknowledges its generation; no reference-count destruction/deallocation on audio. Event storage holds 4096 events per prepared slice; process host queues in bounded sample order where possible rather than blindly copying them. Overflow retains latest value per parameter, guarantees note-off/all-notes-off, increments a diagnostic, and never blocks.

## Coupled simulation contract

Let q(x,T) be electrical stored charge/flux contributions, f(x,T,p) the static KCL/branch residual and u the source vector. Baseline integration is backward Euler:

(q(x_new,T)-q(x_old,T))/h + f(x_new,T,p)-u_new = 0.

Use analytic component Jacobians, scaled partial-pivot LU, previous solution as initial guess, maximum 12 Newton iterations (Eco 6, Studio 8), and four damped step trials per iteration. Accept when both scaled residual infinity norm and relative state-step norm <=1e-7; voltage/current scales come from the circuit manifest, each strictly positive. Cap analytic exponent arguments within the model's documented domain. A manifest must provide q, f, derivatives, domain bounds and calibration provenance; no component name alone constitutes a model.

On failure, reuse the last finite bounded electrical state for one substep, count the failure, and never commit invalid state. Three consecutive failures start a 10 ms fade to latency-aligned dry, preserve the error diagnostic and request an off-thread operating-point reset. Re-entry requires a successfully prepared circuit and explicit Reset or Apply. When no prior valid sample exists, start from delayed dry. No infinite retry or adaptive subdivision on the callback.

Electrical solve runs at Fs*OS. Each host sample aggregates dissipative power from substeps. Reactive stored-energy transfer is not blindly counted as heat; a model manifest identifies dissipative terms, idle/heater losses, and supplied power. Update thermal state at 1000 Hz using a fractional phase accumulator independent of buffer boundaries. For heat capacities C_i [J/K] and thermal conductances G_ij [W/K], solve:

(C_i/dt + G_i,amb + sum_j G_ij)*T_i,new - sum_j G_ij*T_j,new
= C_i/dt*T_i,old + P_i,avg + G_i,amb*T_amb.

This is a proposed coupled backward-Euler heat-network discretization. Require C_i>0, G>=0 and at least one ambient-connected node per thermal island. Factor the constant matrix during preparation; changing conductances prepares another matrix off-thread. Temperatures are absolute kelvin internally and Celsius in UI. Model-specific temperature bounds must be present; their violation produces a diagnostic and clamped evaluation rather than unbounded runaway.

Supply reservoirs obey charge/current balance C*dV/dt=I_rectifier-I_load; rectifier/source impedance and device draw come from the same manifest. The graph compiler either places this in the electrical solve or selects a calibrated reduced supply model explicitly. Rail fraction r=V/V_nom is a control/readout, not a substitute for volts in device equations.

Order per host sample: timestamped events -> macro/condition/modulation targets -> sidechain/transient detectors -> available network frame -> oversampled electrical solve and reservoir -> power accumulation -> due thermal update -> telemetry accumulation -> downsample -> level-matched blend -> output trim -> HPF -> optional soft-clip protection -> emergency containment. At thermal tick boundaries new T affects the following sample; document this one-tick feedback lag in reference comparisons.

Initial state is the converged DC point at ambient with idle losses optionally warmed to equilibrium by the preparation worker. Default "Warm start" uses idle thermal equilibrium, zero stochastic history, and manifest DC capacitor charges. "Cold reset" uses ambient temperature and a prepared startup trajectory. Presets save configuration, not accidental wall-time history; exact-session snapshots are specified in the state document.

Maximum compiled graph: 64 components, 32 electrical nodes excluding ground, 8 nonlinear devices, 96 electrical unknowns, 64 thermal nodes, two channels. These are hard parser/compiler bounds, not performance promises. A graph may be rejected for exceeding its measured per-mode execution budget.

## Oversampling and quality

| Quality | Default OS | Filter | Newton cap | Intended use |
|---|---:|---|---:|---|
| Eco | 4x | polyphase FIR | 6 | many instances |
| Studio | 4x | polyphase FIR | 8 | normal mixing |
| Render | 16x | FIR equiripple linear phase | 12 | explicit high-quality render |

Manual OS choices Off(1x)/2x/4x/8x/16x override the table and are stored in state; 1x and 2x are explicitly labeled Draft quality, exceptions to the requested normal minimum 4x FIR. FIR is the normal phase family; IIR is an optional low-latency family, also marked an exception to the FIR production default. Both apply to the complete nonlinear electrical feedback region including fast rail interaction, not just one waveshaper. The thermal clock stays 1 kHz. No automatic quality reduction during overload and no hidden quality increase in offline export.

Filter-design targets per stage: <=0.01 dB passband ripple, >=100 dB stopband rejection with a transition region chosen and documented by the filter implementation. These filter limits do not prove whole-plugin alias performance. T21 compares the nonlinear result to a 64x double-precision offline reference. FIR/IIR latency and phase tradeoffs follow S04; coefficients/filter order must be frozen and tested before claiming compliance.

Prepare all selected quality changes off-thread. Within one phase family pad every rate to the maximum integer latency among 1–16x at the active Fs; compensate any fractional delay explicitly. Crossfade initialized old/new paths over 20 ms with linear equal-sum weights. Phase-family changes requiring different declared latency are staged until host deactivation/reactivation and latency notification; never silently change latency mid-buffer. A/B and dry paths use the same declared delay. UI labels pending quality separately from active quality.

## Output protection and level-matched blend

Input gain -24..+24 dB default 0; output trim -24..+24 dB default 0; mix 0..1 default 1. Smooth gain/mix with 10 ms time constant. Drive remains a separate nonlinear-stage control from the original vision.

For linked channels compute dry/wet mean-square envelopes after delay alignment and before blend using 400 ms time constant. If dry and wet RMS each exceed -60 dBFS, target compensation is clamp(sqrt((E_d+1e-12)/(E_w+1e-12)),0.25,4). Smooth its logarithm at 1 s. Freeze during silence/noise-only input and reset to unity on Reset. Compensation acts only on wet; enable ramps over 50 ms. Blend y=(1-m)*dry+m*g_match*wet. This is engineering RMS matching, not a promise of perceptually identical loudness. Output trim follows.

Default output HPF enabled at 15 Hz; selectable 10–250 Hz (10–20 Hz is the original DC-protection region), second-order Butterworth Q=1/sqrt(2), bilinear high-pass coefficients using S09. Normalize by a0 and use double-precision transposed direct form II. Recompute coefficients outside the critical loop for static settings; cutoff moves use bounded 20 ms smoothing with stable coefficient calculation. Run filtered/unfiltered paths and crossfade enable over 20 ms. Apply HPF after blend/output trim so leakage in the final output is covered. At dry endpoint this protection and global trims still apply; only host bypass guarantees delayed untrimmed dry.

Independent emergency containment replaces any non-finite sample with zero and flags failure. If an output exceeds absolute amplitude 8, clamp that sample to ±8 and enter the same 10 ms dry fallback; this is exceptional fault containment, not a normal limiter or a guarantee about physical speakers. HPF bypass permits DC; show that state clearly. Host bypass produces latency-aligned original dry, crossfaded over 10 ms, and continues thermal/electrical evolution when the host continues processing.

## Budgets and acceptance

Reference benchmark proposal: Apple M2 performance core and Intel i7-12700K performance core, release build, 48 kHz/128-frame stereo, factory graph, both A/B paths included, GUI/network disabled. Target callback p99 <=0.35 ms Eco, <=1.0 ms Studio; Render <=4.0 ms and may be offline-only on slow hardware. One-instance prepared memory <=64 MiB excluding exporter; active audio stack <=256 KiB. Measure 10 minutes after warm-up and report maximum as well as p99; no average-only CPU claims. Maximum graph and 192 kHz/16x are separate stress tests and may need stricter topology limits.

T20 validates a synthetic thermal node: C=2 J/K, G=0.2 W/K, P=1 W, ambient 25 °C, analytic equilibrium 30 °C and tau=10 s; 1 kHz integration error <=0.002 °C at 10 s. This fixture is not a measured vintage component. Core physical release remains blocked on model manifests and independent calibration listed in OPEN_QUESTIONS.


## UX additions: output ceiling and explicit zero-latency mode

Output soft-clip protection is enabled by default and separate from exceptional containment. Proposed memoryless curve: f(x)=x when |x|<=0.9; otherwise sign(x)*(0.9+0.1*tanh((|x|-0.9)/0.1)). Thus the knee is continuous with continuous first derivative and bounded below absolute 1. Evaluate inside its own prepared oversampling stage at the selected OS rate (normally >=4x FIR), after gain and HPF. Its interpolation/decimation latency is included in the declared total; there is no claim that sample peaks enforce an intersample true-peak ceiling. T23 must check both normal and Draft modes. A user may disable the soft clip for unclipped floating-point output.

Explicit Zero Latency is a separate preparation mode: Off(1x) OS, no lookahead, no creative pitch-delay stage, no audio cross-talk delay, and no padding to HQ latency. Basic circuit processing, causal IIR protection and optional native-rate soft clip remain. UI identifies Draft antialiasing and lists the temporarily unavailable features. Enable/disable Zero Latency is staged through the host latency-change lifecycle, never forced during active processing. Turning off only linear-phase filters is insufficient to claim zero samples.

Full per-instance steady-state memory target is <120,000,000 bytes including attributable CPU/GPU buffers in Easy and Workbench with exporter idle. Report shared framework/driver overhead separately. The user's 1x/Easy CPU target is callback p99 <1.5% of its deadline on each reference system above (at 48 kHz/128, <0.040 ms). This is an aggressive unverified target, additional to the normal-mode budgets. LITE and simplified factory topology are the initial optimization targets. Do not claim this target is met for every edited graph.

Format matrix: VST3/CLAP/AAX Native on Windows x64 and macOS x64/arm64; AU v2 on macOS x64/arm64. AUv3 and AAX DSP are not requested and not promised. Keep core behavior common; adapter tests must verify IDs, state and automation, since wrapper support differs. AAX SDK access/signing and CLAP-adapter pinning are blocking release tasks (S14/S15), not reasons to remove formats.
