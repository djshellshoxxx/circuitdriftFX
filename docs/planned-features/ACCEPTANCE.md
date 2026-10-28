# Acceptance and traceability

All runtime tests below are **specified, not executed**. Limits are proposed engineering gates unless a source is explicitly identified. Runtime harness defaults: deterministic seeds, silence/sine/impulse/noise fixtures, 44.1/48/96/192 kHz, mono/stereo, block partitions {1,17,64,127,512,8192}; compare signals after declared latency alignment. Record compiler, CPU, model/filter revisions and exact command. Host qualification additionally covers 88.2/176.4 kHz.

| Test | Feature | Required acceptance |
|---|---|---|
| T03 | CD-F03 | Decode PNG/GIF/MP4; exact selected dimensions, GIF 50 ms/frame, MP4 30 FPS; A/V skew <=33.4 ms and 60 s drift <=20 ms; bounded queues, audio unaffected by stall/full disk |
| T04 | CD-F04 | Mock API tests cover all status codes, schema/digest rejection, auth ownership, idempotent publish and offline cache; no credential/state leakage and no network on audio |
| T05 | CD-F05 | Exactly two primary knobs; equivalent full-scene render peak error <=1e-6; offline launch/restore succeeds; format IDs distinct |
| T06 | CD-F06 | Macro endpoint mappings within 1e-6 normalized; no writes into derived host parameters; blocks partitioned differently agree <=1e-6 peak |
| T07 | CD-F07 | 75/100/125/150/200% in both themes and OS DPI scales; no clipped essential controls; all sonic actions keyboard/numeric accessible |
| T08 | CD-F08 | Every shipped model has bundled tooltip, correct role/material/units and source; opens after 450 ms ±one UI frame; no network required |
| T09 | CD-F09 | 20 worst-case transactions undo/redo to canonical config equality; new edit clears redo; automation excluded; history <=56 MiB |
| T10 | CD-F10 | Malformed graphs, unsupported pins/models, singular circuits, invalid resources and over-budget probes rejected; active graph unchanged; valid swap one transaction with bounded transition |
| T11 | CD-F11 | 2/32 members, isolated sessions, reordered/missing callbacks and broker death; zero callback waits; absolute cross-talk row sum <=configured gain; replay peak error <=1e-6 |
| T12 | CD-F12 | Every allowed scale maps within 0.01 cents of selected target after settling; ties/hysteresis deterministic; clean mono tones 80–1000 Hz estimated within 15 cents after acquisition; low confidence/chords never advertised certain |
| T13 | CD-F13 | Isolated first-order 95% recovery time within 1% of selected beat duration at 60/120/180 BPM; phase continuous on tempo change; missing PPQ/BPM fallback visible |
| T14 | CD-F14 | Calibrated fixture even-versus-odd H2/H3 ratio shifts >=6 dB in intended direction, without invalid/DC-overbound state; unavailable model map disables feature |
| T15 | CD-F15 | At amount 0 difference <=1e-7 peak; full preservation reduces first 10 ms sag attenuation >=50% in kick fixture with >=3 dB unprotected loss; physical reservoir trajectory unchanged |
| T16 | CD-F16 | Depth 0 identity <=1e-7 peak; calibrated external input produces expected current <=1% error; disconnected bus gives zero extra current and status; sidechain never leaks as output audio |
| T17 | CD-F17 | Telemetry matches simulated values within float conversion tolerance, correct units and generations; GUI close/stall does not change deterministic audio |
| T18 | CD-F18 | Anchor/interpolation values within 1e-6 relative; arbitrary back-and-forth scrub returns identical condition; restoration identity changes only on committed action |
| T19 | CD-F19 | Every supported fault at 0/0.5/1 for 60 s silence/drive remains finite; repeated seeded patterns match; recovery leaves no stale fault overlay |
| T20 | CD-F20 | Thermal analytic fixture error <=0.002 °C at 10 s; DC/KCL residual <=solver target; one-hour stress finite; actual hardware accuracy requires G01 reference tests |
| T21 | CD-F21 | OS rates exact; stage ripple <=0.01 dB/stopband >=100 dB; Studio/Render coherent multitone unwanted aliased-bin energy <=-70/-85 dB relative to reference output against 64x reference, 20 Hz–20 kHz or 0.45Fs band; publish stimulus/bin exclusion method |
| T22 | CD-F22 | 15 Hz Butterworth corner -3.0103 dB ±0.05; DC step residual <-100 dBFS after 2 s; enable transition meets defined ramp; bypass clearly allows DC |
| T23 | CD-F23 | Gain ranges correct ±0.01 dB; identical unity wet/dry mix deviation <0.01 dB; stationary RMS match ±0.5 dB after 5 s within gain bounds; clip curve bounded and continuous |
| T24 | CD-F24 | Identical-slot mix unity <0.01 dB; slot swap sound invariant <=1e-6 peak; preset/config roundtrip exact; compatible dynamic checkpoint continuation <=1e-6 |
| T25 | CD-F25 | Measured quality rates/latency exact, no hidden downgrade, budgets in runtime met on named hardware or claim blocked; active/pending quality always distinguishable |

## UX coverage

| ID | Description | Test |
|---|---|---|
| UX-01 | PCB gestures and tools | Tube 200 logical-pixel drag spans normalized bias; pinch ratio sqrt(2) doubles C within bounds; Escape rollback; one undo and coherent host edits |
| UX-02 | Two views/four macros/quick scenes | Mode toggle produces bit-identical audio; factory macros match declared values; settings persist through resize |
| UX-03 | Lifecycle scrubbing | T18 plus correct alternative labels and separate Restored/Derelict outcomes |
| UX-04 | Thermal XY/temperature/rail/B-H | Unity fixed-scale diagonal <=0.5 pixel error; 0.5-gain slope within 1%; units correct; B-H unavailable without H/B |
| UX-05 | Rail/meter display | 50 ms reference recovery shown within 2 ms; AC/DC labels distinct; gaps/stale frames marked |
| UX-06 | Phase/coherence scatter | Known delay phase within 3 degrees in coherent bins; independent noise has low coherence; mono N/A; DC readout separate |
| UX-07 | Tone State Sphere | Values follow specified visual mappings; smooth silence; reduce motion; render RNG never changes audio RNG |
| UX-08 | Utility and corrected math | T13/T20/T21/T22/T23 plus ±24 dB, 10–250 Hz and draft-quality labels |
| UX-09 | Realtime/APVTS bridge | Instrument allocation/lock/OS I/O traps on audio; zero violations over 1-hour stress; validated SPSC ownership |
| UX-10 | Targets/accessibility/formats | GUI p95 60 FPS budget, memory/1x CPU targets measured; AU/VST3/AAX/CLAP state/automation/latency adapters qualified |

## Package verification versus runtime verification

The implemented script tools/validate_planned_features.py checks documentation link targets, complete feature/test coverage, parameter IDs/defaults/ranges, JSON schema/example validity and simple independently calculated design formulas. These checks validate this specification package only. It does not compile a plugin, measure audio quality, benchmark graphics or certify physical models.

Every implementation task must cite the relevant test and source/model fixture; a test marked blocked by missing G01/G03 data cannot be converted to Passed using invented numbers.
