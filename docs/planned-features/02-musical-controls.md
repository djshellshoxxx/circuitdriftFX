# Macros and musical modulation

Planned contract for CD-F06/F12/F13/F14/F15/F16 and UX-02/UX-08. Tests T06/T12/T13/T14/T15/T16. Sources S02/S05/S12 in [SOURCES](SOURCES.md). Numerical mappings here are authored creative design choices, not universal device laws.

## One-way parameter composition

Each A/B slot stores base physical parameters b, macro drivers m, timeline condition c and a stable variation seed. Host automation edits those stored controls; the audio thread computes effective values e without writing derived values back to APVTS or other host parameters. GUI displays Base and Effective separately. Mode switching changes no state. A macro never calls setValueNotifyingHost on its targets. This avoids competing host automation (S02).

Evaluate in order: base -> timeline -> Age/Sag/Warmth/Drift contributions -> musical modulation -> selected faults -> model-domain clamp. Continuous controls use 20 ms smoothing in their declared domain; topology/enum changes are transactions. Macro values are global across both A/B paths; blueprint/base condition is per slot. Each slot derives independently. These ownership rules are canonical; macro slots cannot silently become per-slot later.

Factory macro mappings, all normalized a,s,d,w in [0,1]:
- Age: effective service age max(baseAge,60*a) years; add tolerance standard-deviation contribution 0.08*a, bounded by the manifest and global ±25% envelope; add leakage fraction 0.1*a^2 of the manifest's declared safe leakage range. Add high-frequency tilt using a -6*a dB shelf at 6 kHz, Q=0.707. This shelf is creative, not a capacitor-aging equation. Noise added by Age is disabled unless Analog Noise is enabled; its target is -90+24*a dBFS at the calibrated factory input.
- Sag: effective reservoir C=max(C_min,C_base/(1+3*s)); extra requested load multiplier=1+3*s. At s=0 the macro adds no sag; independent base circuit droop may remain. Physical charge integration remains active.
- Warmth: add 18*w dB pre-stage drive; bias offset +0.08*w of the manifest's allowed bias span; transformer flux drive factor 1+0.5*w within its safe domain. Auto-gain compensates only when enabled.
- Drift: multiply measured temperature-to-parameter sensitivity by 1+4*d; add creative low-frequency cutoff motion up to ±0.15*d octaves and optional delay deviation up to ±1.5*d ms. d=0 removes creative motion; natural base thermal drift remains unless the Base Drift coefficient is also zero.

Macro endpoints are contributions, not a claim that arbitrary damaged Workbench settings become transparent. The Clean Console factory preset explicitly disables noise/leakage/faults and uses the calibrated clean graph. Editing a detailed control changes its base; do not reverse-solve macro values. Show Modified when a quick preset no longer matches.

Model manifests must define C_base/C_min, voltage/current/bias/flux scales and calibrated device sensitivity. Missing bindings make a blueprint incompatible with a macro, visibly disabled for that slot; no guessed units. A published custom blueprint must expose required macro bindings or declare unsupported macros.

## Actual thermal state versus creative stochastic drift

Keep physical temperature from the heat ODE. Do not add unitless RMS or pink noise directly to kelvin. The author's temperature sketch is replaced by that physical state plus a separately labeled creative control process.

Reproducible creative motion uses a 100 Hz control clock and seeded Gaussian white innovations. Three stationary processes u_j[k+1]=a_j*u_j[k]+sqrt(1-a_j^2)*z_j[k], with tau={0.2,2,20}s and a_j=exp(-0.01/tau_j), are combined as q=tanh((u0+u1+u2)/sqrt(3)). This is bounded multiscale colored motion, not claimed exact pink noise. Seeds are derived from instance seed, slot, component UUID and stream type; sample-clock advancement is block-independent.

For optional pitch movement, use an 8 ms preallocated delay ring, cubic interpolation, base delay 1.5 ms, deviation 1.5*d*q ms and slew limit 0.002 seconds/second. Report base delay plus interpolation/oversampling latency and align dry/A/B paths. This produces time-varying pitch; it is explicitly creative, not asserted as an automatic consequence of sag. Disable it in Zero Latency mode. Prepare/latency handshake applies to enabling it.

## Key/scale tracking (CD-F12)

Sources: Manual, MIDI, Audio. Root=0..11, reference A4=440 Hz, scales Chromatic, Major, Natural Minor, Pentatonic Major/Minor, Custom 12-bit mask (nonzero). Scale intervals respectively {0..11}, {0,2,4,5,7,9,11}, {0,2,3,5,7,8,10}, {0,2,4,7,9}, {0,3,5,7,10}. Manual root remains authoritative; incoming note selects reference octave/pitch. MIDI last-held note wins; sustain retains note until pedal release; all-notes-off releases immediately; no active note holds reference 500 ms then returns to manual A4-centered octave over 100 ms.

For a target resonance f, compute p=69+12*log2(f/440), select nearest allowed integer MIDI note across valid filter octaves; ties go to lower note. Apply hysteresis 0.15 semitones around boundaries and glide 50 ms in log-frequency. Quantize only creative resonance/cutoff movement; do not quantize internal kelvin, rail voltage or arbitrary electrical states. For delay-based pitch drift, scale lock ramps delay movement to zero and labels "Resonance lock; pitch drift held." It is not a polyphonic pitch corrector.

Audio source is explicitly monophonic. Proposed normalized autocorrelation detector: downsample with antialiasing to 12 kHz on a worker, 1024-sample Hann window, hop 128; search lags 6..218 (approximately 55..2000 Hz), rho(l)=sum x[n]x[n-l]/sqrt(sum x[n]^2*sum x[n-l]^2+epsilon). Use peaks above 0.8, prefer the smallest lag whose score is within 0.03 of the maximum, refine parabolically, reject RMS<-60 dBFS and octave jumps unless stable for 3 hops. Two valid hops acquire, 200 ms invalid holds then reverts to manual. Worker capacity: 8 windows, drop old windows on overload. Timestamp results; reject >250 ms stale. This proposed detector requires calibration tests; S12 is a YIN research lead, not a claim this implementation is YIN. Chords/noisy signals show Low confidence rather than a fabricated key.

## Tempo breathing (CD-F13)

Division enum: 1/32,1/16,1/8,1/4,1/2,1 bar; modifiers straight/dotted/triplet. For q quarter-note beats, D=60*q/BPM seconds, tau_recovery=D/ln(20), giving 95% recovery in D for an isolated first-order response. Bars use time signature numerator*4/denominator, default 4/4 only when unavailable. BPM clamp 20..300; unavailable BPM uses explicit 120 fallback and indicator (S05).

In Sync mode scale a calibrated recovery control or virtual charge resistance to obtain the target response; this is creative retiming and does not alter component heat capacity to match a beat. Offer additional sinusoidal creative drift modulation depth 0..1 on phase=frac(PPQ/q); free-run by sample clock if PPQ missing. Tempo changes preserve continuous phase and smooth recovery coefficient over 50 ms. Seek/loop resets phase from PPQ; no jump in audible state, with 20 ms target smoothing. An RLC circuit may not have single-exponential recovery; displayed measured recovery is authoritative and the requested tempo target is an approximation.

## Harmonic targeting (CD-F14)

Target control -1..+1: -1 Odd, 0 Neutral, +1 Even; amount 0..1. It selects a calibrated bias/drive operating point, not additive synthetic harmonics and not guaranteed isolation of an order in arbitrary music. Offline calibration per model sweeps 41 bias points x17 drive points at 100 Hz/1 kHz, -24/-12/-6 dBFS, captures harmonics 2–9 after settling and rejects unstable/DC-excess points.

Let R=20*log10((H2+1e-12)/(H3+1e-12)); interpolate a monotonic safe branch of the bias/drive map using target R between the measured attainable endpoints. Amount interpolates from manual base point to that point. Bound drive change to ±6 dB and bias to the manifest range; smooth 50 ms. No audio feedback optimizer chases live music. If a model lacks a calibration map, disable targeting and identify that dependency. T14 requires at least a 6 dB directionally correct H2/H3 ratio change for the calibrated factory fixture; otherwise narrow the advertised range instead of inventing a map.

## Transient recovery and external sidechain (CD-F15/F16)

Causal linked detector at host Fs: p=max(abs(L),abs(R)); fast envelope attack 0.2 ms/release 10 ms, slow attack 20 ms/release 100 ms. Attack evidence b=clamp((20log10((fast+1e-9)/(slow+1e-9))-3)/9,0,1), gated below -60 dBFS. Hold its peak for 5 ms then release 20 ms; preservation strength P=amount*b. Use r_effective=r+P*(1-r) for headroom/bias control. Physical reservoir charge r continues unchanged, avoiding an unexplained energy refill. This is intentional creative transient protection, not perfect zero-lookahead attack preservation. With shared PSU it changes only the receiving member's effective behavior.

Sidechain source Main, External, Blend. External missing -> zero external load plus status; main idle and signal draw continue. Link by max channel magnitude, optional sidechain HPF 20..300 Hz default 40, attack 1 ms/release 50 ms load detector. I_ext=depth*I_budget*clamp(E_ext/E_ref,0,4), where E_ref corresponds to -18 dBFS RMS and manifest I_budget is in amperes. Depth 0..2, default 0. This detector controls an extra current source into the capacitor differential model; it does not replace capacitor integration with a compressor envelope. Blend load is main plus external contribution, never sidechain audio mixed into the output. Bound by rectifier/rail model domain and preserve minimum rail manifest bound. T16 checks disabled bus, external silence, calibrated sine and kick, and amount=0 identity.


Sidechain selector precedence is explicit: let I_phys be the main circuit's real current draw and I_dyn its nonnegative signal-dependent excess over idle. Main uses I_phys+3*s*I_dyn; External uses I_phys+I_ext; Blend uses I_phys+3*s*I_dyn+I_ext. The core's physical main-path current is never removed. Thus External replaces only the creative main-load augmentation, and differs meaningfully from Blend. This formula defines the previously described load multiplier without double-counting the current already supplied by the electrical solve.
