# Control bug pass G3 — 2026-11-19

Execution date: 2026-11-20 UTC. Local synthetic fixtures only; no external testing, remote comments, or pushes.

## Scope and base

- Base commit: `332343ce3c412db0e0dd9c692a5d017889c76f8f`.
- Branch: `codex/cdfx-control-bugs-g3-20261119`.
- Worktree: `/workspace/scratch/7a10b82b2964/cdfx-control-bugs-g3`.
- Main-patch production edits are limited to `CreativeDrift.*` and `ControlProcessor.*`; the separately authorized follow-up also touches the tempo input boundary.
- Added the `control_safety` CTest target and its synthetic regression fixtures.
- Read `CLAUDE.md`, the planned-feature read order, musical controls contract, and relevant decisions/acceptance gates before editing.

## Findings and fixes

### Invalid drift snapshots and unbounded historical catch-up

Previously, `CreativeDrift::restore()` copied all fields without validation. Nonfinite process values, invalid Gaussian cache flags, an all-zero xoshiro state, and inconsistent sample/tick counters were accepted. A later small `advance()` could attempt to process a checkpoint's entire historical tick backlog.

Restore now validates all fields before mutating any state. It checks finite output/process/cache values, bounded delay/output domains, a nonzero RNG state, valid cache flags, output consistency with the three processes, and exact sample/tick consistency for the prepared rate. Invalid snapshots leave both state and future RNG output unchanged.

The Gaussian cache bound is 9: the smallest generated uniform is `2^-54`, so Box–Muller magnitude is less than `sqrt(108*ln(2)) < 9`. For the three AR coefficients, `9*sqrt(1-a*a)/(1-a) < 570`. The accepted process bound of 1024 is therefore conservative and invariant under subsequent valid updates; it rejects finite values large enough to create future overflow.

The clock now represents `100/fs` as a reduced integer ratio derived exactly from the binary64 sample rate. Advances skip to each next control tick, consuming at least one supplied frame per iteration. Snapshot validation derives the phase and tick count using 64 fixed integer steps, without running stochastic ticks. This preserves block partition independence at ordinary, fractional, and very large sample positions. The sample counter saturates at `UINT64_MAX`; further advances hold state instead of wrapping or accumulating backlog.

The arithmetic uses standard `uint64_t`, `frexp`, `ldexp`, and `gcd`. Supported-rate exponents bound phase intermediates below `2^55`. There is no dependency on `__int128`, compiler overflow builtins, or `long double` being wider than `double`. A separate worker performed a read-only arithmetic review and found no defect.

### Parameter conversion order

Enum values were rounded/converted before clamping. The custom scale mask was converted to `int` before clamping. Extreme finite direct-call parameters could overflow those conversions or wrap to a different choice.

The code now applies the finite fallback and floating-point domain clamp before rounding or integer conversion. Existing in-range enum rounding and custom-mask truncation behavior are preserved.

### Seeded reset and preparation

`ControlProcessor::reset()` previously left both creative drift generators and their clocks running. It now restores the seed from the latest successful preparation for both independent slot streams, resets source/tempo caches, and reproduces the modulation sequence of a fresh processor with the same configuration.

Both control preparation boundaries now accept finite sample rates in the runtime's existing 8000–768000 Hz supported interval and use 48000 Hz otherwise. This prevents tiny/nonfinite/huge preparation rates from entering clock or pitch-detector integer conversions.

## Verification

Environment: Linux, GCC/G++ 14.2.0, C++20. Builds used `-O0 -g0`, one compilation job, and ASan/UBSan/float-cast-overflow. Only control targets and their existing library dependencies were built; no redundant full-project build was performed.

Configuration:

```sh
/workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/cmake -S . -B build-control-g3 \
  -DCD_BUILD_PLUGIN=OFF -DCD_SANITIZE=ON \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS_DEBUG='-O0 -g0' -DCMAKE_CXX_FLAGS_DEBUG='-O0 -g0'
```

Regression proof before the production fixes:

```sh
/workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/cmake --build build-control-g3 \
  --target cd_control_safety_tests -j1
timeout 30s env UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 \
  build-control-g3/tests/control/cd_control_safety_tests
```

Result: exit 1. Multiple malformed-snapshot rejection assertions failed. UBSan then stopped at the original `ControlProcessor.cpp:166` custom-mask conversion: `1.79769e+308 is outside the range of representable values of type 'int'`. The rejection fixtures restore their valid baseline between mutations and deliberately avoid executing the unfixed malicious backlog.

Post-fix build and all control suites:

```sh
/workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/cmake --build build-control-g3 \
  --target cd_control_safety_tests cd_control_dynamics_tests cd_control_key_tests \
    cd_control_tempo_tests cd_control_macros_tests cd_control_harmonic_tests \
    cd_control_pitch_tests -j1
env UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 \
  /workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/ctest \
  --test-dir build-control-g3 -R '^control_' --output-on-failure -j1
```

Result: **7/7 passed**, 23.42 seconds; no sanitizer report. Coverage includes the existing macro, key, pitch, tempo, harmonic, and dynamics suites.

The new safety suite covers transactional rejection and future continuation; finite conversion extremes, NaN, and infinity; reset and re-prepare determinism; invalid preparation rates; block partitions and checkpoint continuation at 8/44.1/48/88.2/192/768 kHz plus 44100.5 Hz; and coherent clocks near `UINT64_MAX` with one-frame updates and saturation.

A final fixture was added for the full-significand rate `nextafter(44100.0, 50000.0)`. It verifies that sample 44100 has only 99 ticks, sample 44101 reaches tick 100, checkpoint continuation preserves the boundary, and a large exact-rational boundary advances correctly. Only the changed safety target was rebuilt and rerun:

```sh
/workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/cmake --build build-control-g3 \
  --target cd_control_safety_tests -j1
env UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 \
  /workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/ctest \
  --test-dir build-control-g3 -R '^control_safety$' --output-on-failure -j1
git diff --check
```

Result: **1/1 passed**, 0.08 seconds; whitespace check passed.

## Open work and limits

- Windows/MSVC execution was not available locally. The code and tests avoid nonportable extended integer/float assumptions; the parent integration should run the Windows CI gate.
- Compatible checkpoints now require a coherent exact tick count for their prepared rate. A snapshot already made inconsistent by old floating-point clock rounding is rejected; no migration of such corrupted/inconsistent snapshots is supplied.
- The initial pass identified unchecked reference tuning and nonfinite transport PPQ as direct-call boundary follow-ups. The parent subsequently authorized the separate fix recorded below.
- This pass does not cover pitch-worker concurrency, runtime latency handshakes, engine/DSP/state/cache/plugin code, or hardware/model qualification. G01 physical validation remains open.

## Separately authorized follow-up: tuning and transport inputs

After the main patch was committed as `7cdb7f5ae7e4bc5de2257bdb75e5d4687934f638`, the parent requested fixes for the two concrete adjacent direct-API issues. This follow-up adds only the following production changes:

- `ControlProcessor::setReferenceTuning()` maps nonfinite/nonpositive inputs to 440 Hz. Ordinary positive tuning keeps its existing pitch calculation. When the ratio overflows for an extremely small positive tuning, a difference of logarithms keeps the resulting pitch finite before scale quantization.
- `TempoBreathing` treats nonfinite optional BPM/PPQ as unavailable. For finite PPQ, it reduces the beat position modulo the cycle before dividing, so a very large finite PPQ and a short cycle cannot create an infinite cycle count and poison the phase. Negative PPQ retains its wrapped phase behavior.

The new tuning fixture was first run before these production changes. UBSan stopped at `ScaleQuantizer.cpp:33`: `nan is outside the range of representable values of type 'int'`. That verifies the original invalid tuning reached the integer quantizer.

The follow-up regression cases cover NaN, both infinities, zero/negative tuning, positive subnormal/minimum/maximum doubles, ordinary 432 Hz tuning, nonfinite BPM/PPQ fallback, valid transport continuation after invalid metadata, both signs of maximum finite PPQ, and a negative fractional PPQ phase.

Using the same sanitizer build configuration:

```sh
/workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/cmake --build build-control-g3 \
  --target cd_control_safety_tests cd_control_key_tests cd_control_tempo_tests \
    cd_control_macros_tests -j1
env UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 \
  /workspace/scratch/7a10b82b2964/tooling/cmake/data/bin/ctest \
  --test-dir build-control-g3 -R '^control_(safety|key|tempo|macros)$' --output-on-failure -j1
git diff --check
```

Result: **4/4 passed**, 0.60 seconds; no sanitizer report. Existing key/tempo/macro behavior remains covered. The earlier seven-suite result applies to the main patch; only these four affected suites were rerun for this follow-up.
