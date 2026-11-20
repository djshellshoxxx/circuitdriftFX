# PitchDetector direct-prepare bounds — isolated engineering handoff (2026-11-20 UTC)

## Exact source and isolation

Base: draft PR #16 at `b3e05b652147a584ad5fb22e5715d1e61b702318`. Branch: `codex/cdfx-pitch-prepare-bounds-20261120`.
The PR #16 coordinator owns runtime/session integration and exact-head
Windows/pluginval; the separate preset branch owns live library services.
This continuation changes only the detector's direct preparation boundary,
its existing pitch regression suite, and isolation documentation.

## Reproduced source-level defect and repair

`PitchDetector::prepare` accepted nonfinite or arbitrarily large rates after
a minimum-only check, and repeatedly doubled its `std::size_t` ring capacity
until it reached the caller's request. For an extremely large size such as
`SIZE_MAX`, unsigned doubling can wrap to zero, never satisfy the bound,
and loop without progress. The previously accepted positive infinity could
also overflow tap counts during conversion.

Reject nonfinite rates and rates outside 16,000..768,000 Hz (the existing
`ControlProcessor` upper bound), and reject requested ring capacities above
2^20 samples before allocation or power-of-two rounding. Preserve the prior
minimum effective ring size of 1024, including a zero-size request.

A second state-consistency finding: `reset()` reset the generation counter to
zero. A saved result from a preceding prepared session could therefore match a
later prepared session with the same rate. Invalid reprepare also left the old
published result visible and freshness did not check `prepared()`.
Reset now advances the epoch; rejected prepare clears the published result and
producer counters; `isFresh` rejects unprepared and future-timestamp results.
All reset/prepare operations retain the idle-only ownership requirement.

## Added regression cases

`tests/control/test_pitch.cpp` adds `directPrepareBounds()`:
- NaN, +/- infinity, negative/zero, below-minimum and above-maximum rate rejection;
- `SIZE_MAX` and 2^20+1 ring rejection, and both inclusive boundary accepts;
- saved pitch invalidation on same-rate reprepare and rejected reprepare;
- unprepared producer ignores audio; normal post-reset reacquisition.

Run, without relaxing existing thresholds:

```sh
cmake -S . -B build-pitch -DCD_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-pitch --target cd_control_pitch_tests --parallel 1
ctest --test-dir build-pitch --output-on-failure -R '^control_pitch$'
```

## Verification, independent review and integration gate

Connector patch preflight checked exact source anchors, JSON encoding and
balanced delimiters. No native compile, CTest, sanitizer or Windows/pluginval
execution is claimed in this isolated run: GitHub repository access is via
the connector and the execution container cannot resolve github.com to
obtain full CMake sources. No fresh code-review, bug-hunt or security-review
workers were available; those reviews must independently inspect this
change and run the targeted tests. Check the live PR head before adapting
or integrating; never rewrite coordinator-owned branches.

Remaining ownership: complete dynamic checkpoint/seed/session persistence,
atomic dynamic A/B state handoff, older PR #9/#10/#14 semantic reconciliation,
and host/plugin qualification. All pitch fixtures are synthetic.
