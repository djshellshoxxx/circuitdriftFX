# Controls and checkpoint continuation — rounds 29–33

The current work follows the published host-state restoration checkpoint in
[PR #16](https://github.com/djshellshoxxx/circuitdriftFX/pull/16). It preserves the
original branches and `main`. The [machine-readable execution record](../evidence/control-checkpoints-2026-11-20-r30-r33.json)
contains source/tree identities, commands, individual durations and linked raw
logs/JUnit. All fixtures here are synthetic; physical calibration remains G01.

## Repaired control behavior

Three R30 workers independently owned MIDI boundaries, key/scale behavior and
sidechain dynamics. Each supplied test-only and implementation commits. Root
integrated the tests first and executed the failing baseline against unchanged
production source, then integrated the repairs. All three workers completed
before the fresh R31 review set began.

### Ignored MIDI preserves the processing trajectory

The runtime previously subdivided callbacks at irrelevant MIDI events. Controls
are sampled at block ends, so those extra boundaries could change sound even
when the event should have no effect. Admission now retains key-tracking note
events and CC64/CC120/CC123 only in Full MIDI mode. Manual, Audio and LITE modes
do not create MIDI-driven control boundaries. Disabled scale lock still tracks
MIDI; it is not used as a source-admission gate.

The bounded 1,024-event contract and observable raw overflow remain. Overflow
reserves a terminal all-notes-off event, including when the other admitted events
are irrelevant. Paired runtime fixtures compare rendered samples, targets and
state against the same input without ignored events. They do not establish
audio-pitch acquisition because the fixture does not run that worker.

### Scale changes retain glides and choose reachable notes

Scale/root changes used to invalidate both the decision and initialized output,
causing the next update to snap. Separate decision and output state now preserves
the current glide position. The regression independently checks 70→69 over the
configured 50 ms, including a zero-time reconfiguration and interrupted glide.

The controller now selects allowed notes inside the same effective cutoff domain
used by the engine. The base first observes the sample-rate ceiling; creative
modulation then applies within the declared ±2-octave bound. This avoids choosing
a note that the downstream clamp would move outside the scale. Eight runtime
cases use independent allowed-note enumeration and a direct-engine audio oracle.
Extreme positive finite A4 tuning, including subnormal values, remains finite.

The engine's continuation algorithm changed, so its checkpoint identity now uses
revision `C0DE0002`. A frozen calculation of the prior `C0DE0001` identity is
rejected without changing state. That regression constructs an owned record in
the current shape; it is not a historical-binary transfer test.

Live changes to the base frequency have a separate remaining trajectory question:
the controller sees the requested base while the engine smooths it over 20 ms.
No executed transition test in this record establishes continuity for that edit.
The engine's safety domain follows its actual smoothed base; the concern is not
a newly demonstrated violation of that numeric domain.

### Sidechain histories survive zero-frame updates

Zero-frame calls now apply the normalized HPF setting while preserving both
channel histories, mean square, envelope and last ratio. Positive-frame Main or
disconnected calls still produce zero current and reset signal history.

Nonfinite or excessively large input contributes silence before touching filter
history. Prospective filter output and states are checked before commit; accepted
magnitudes are bounded by `2^500`, allowing finite squaring. Budget and final
multiplication bounds keep current finite at maximum depth and ratio. Regressions
cover unchanged future history, invalid input versus a silence reference, stereo
symmetry, large finite values, zero depth, maximum budget and normalized rates.

These are direct/control-runtime positive-budget fixtures. The plugin adapter
still supplies no positive model budget, so this work does not claim audible
external sidechain load in the plugin. The R30 suite does not independently
force every prospective-filter rejection branch.

## Fixed-size snapshot feature

The [control snapshot API](../engineering/CONTROL_SNAPSHOTS.md) composes six typed
component states into one bounded value: drift, tempo, key tracking, quantizers,
transient detection and sidechain load. It captures slot blends/cached targets,
status and pending settings. Candidate validation precedes every live write.

The first aggregate supports Manual/MIDI with no pending, in-use or retained
harmonic map. Audio selection is explicitly unsupported even while disabled.
Version, effective sample rate, maximum block and seed/reset origin must match.
The caller retains parameters and future input/callback ownership. Pitch-worker
state, map pointers and scratch sample buffers are excluded. Host persistence,
coordinated engine/control capture, dynamic A/B and full G12 remain unfinished.

### R32 component handoff and correction

All three R32 workers completed. The code and defensive security auditors reviewed
immutable source `3bbfbce25a0905464ac4b9671f215a9b429dd60c`, tree
`222cea284311dfb216789fc1af925d445dccfaa6`, without executing code. Both found the
same defect: drift snapshots lacked rate identity. Historical tick counts can
coincide at 48 kHz and 44.1 kHz while future tick boundaries differ.

The independent test worker committed `334ab0571020ea3cc22496e0f1ee19d693fe1441`.
Root executed it before repair and reproduced exactly 28 assertions from the two
fresh/17-sample cross-rate rejection cases. No other case failed. The correction
`18b5d8f7cd062daaeb6dc9a4f8cfeeab3e0cfe69` adds effective sample rate to the saved
state and admission. Same-rate transfer across seeds and slots remains supported.
The fieldwise comparator and malformed-rate cases also include the new metadata.

The component suite compares exact future outputs and field values for identical
future calls. Every rejected mutation gets an untouched live reference and its
own future replay, avoiding a later successful restore that could hide partial
mutation. It covers legitimate stale caches, inactive timers, pending quantizer
decisions, retained glide fields and zero-frame HPF history. Ordinary/nothrow C++
`new`/`new[]` counters are calibrated; they do not intercept every allocator,
locking operation or I/O mechanism.

KeyTracker's independent snapshot admission is limited to pitches within ±32768,
which covers processor-produced values. Direct callers supplying larger finite
audio-reference pitches are outside that snapshot contract. No blanket guarantee
for every finite direct `advanceAudio()` argument is claimed.

## Root-executed results

GCC 14.2, CMake/CTest 4.4.5, `-O1 -g0 -DNDEBUG`, AddressSanitizer,
UndefinedBehaviorSanitizer and float-cast-overflow checks. Leak detection is
disabled under the execution environment; undefined behavior halts the run.
One compiler and serial suites are used. No test threshold was relaxed.

| Source | Executed scope | Result |
|---|---|---|
| `b7ee32b` | Test-only control/runtime regressions; production unchanged from `1a3f8f3` | Expected failure: 129 assertions across 3 suites, 15.03 s |
| `00e2957` | Eleven engine/control/runtime suites after the three repairs | 11/11 passed, 129.24 s |
| `e8b093a` | Legacy engine algorithm-ID admission | Expected failure: 2 assertions, 0.90 s |
| `bef8d07` | Engine checkpoint rejection and unchanged-state regression | 1/1 passed, 0.88 s |
| `921240b` | Independent six-component snapshot suite before the rate fix | Expected failure: 28 assertions, 0.06 s |
| `3fb0846` | Fixed component suite plus nine affected existing suites | 10/10 passed, 21.15 s |

The last row compiles the aggregate API but does not execute its new aggregate
behavioral suite, which is still being written by the fresh R33 bug/test worker.
R33 code and defensive security reviews are also active on source `09f0024`, tree
`5cd5509a897de11bced6a07acc8d3fb3faea2762`. Root owns build/test execution and
integration. All six native plugin/editor targets rebuilt with Clang 19.1.7,
Custom `-O0 -g0 -DNDEBUG`, pinned JUCE and no sanitizers; **6/6 passed in 57.40 s**
at the same source 3fb0846. This checks existing processor/editor integration
after the control changes and ABI additions. It does not independently exercise
the new aggregate API. Neither this native result nor R30's eleven-suite run
is relabeled as aggregate behavioral qualification.

Windows validator fixtures, real two-product pluginval, real-host/performance
qualification and the separate open feature gates retain their prior limits.
