# CircuitDrift benchmarks

`cd_bench` (benchmarks/bench_engine.cpp) times `Engine::process` per 128-frame stereo callback at
48 kHz on the factory SYNTHETIC diode-clipper graph (Eco, Studio, Render, 1x Zero Latency) and
prints p50/p99/max. It is not a ctest test and never links into product code.

## Commands

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target cd_bench cd_tests cd_solver_safety_tests --parallel 4
./build-rel/cd_bench            # 2000 warm-up + 20000 measured callbacks per case
```

Profiling used here (perf is not installed on this VM):

```sh
valgrind --tool=callgrind --callgrind-out-file=cg.out ./build-rel/cd_bench 100 0
callgrind_annotate cg.out
```

## P17 performance pass: before/after (2026-11-11)

**Machine:** `Intel(R) Xeon(R) Processor @ 2.10GHz` (the CPU string cd_bench prints), 4 vCPU.
This is a **shared VM, not reference hardware**. Two other agents and a long-running stress test
were using it (load average 1.5-7.9 during the runs). **No spec target is claimed met or missed
from these numbers**, and the fixture is synthetic (G01 stays open).

Method: the pre-change binary (commit b9e2aa4) and the post-change binary were run alternately,
3 runs each, default arguments, normal priority. Values are ms per callback. Min and median are
over the 3 runs. The max column is set by scheduler interference on this VM; do not read it as
a property of the code.

| Case | Metric | Before min | Before median | After min | After median | Median change |
|---|---|---|---|---|---|---|
| Eco | p50 | 0.4379 | 0.4436 | 0.2821 | 0.2902 | -35% |
| Eco | p99 | 0.6757 | 0.6822 | 0.3600 | 0.4916 | -28% |
| Eco | max | 2.3793 | 2.8508 | 1.2932 | 2.4712 | -13% |
| Studio | p50 | 0.4263 | 0.4380 | 0.2904 | 0.2939 | -33% |
| Studio | p99 | 0.6502 | 0.7017 | 0.4629 | 0.5111 | -27% |
| Studio | max | 2.8171 | 3.1252 | 2.4153 | 2.6765 | -14% |
| Render | p50 | 1.5605 | 1.5637 | 0.9987 | 1.0124 | -35% |
| Render | p99 | 2.3230 | 2.3800 | 1.4481 | 1.6473 | -31% |
| Render | max | 4.8595 | 4.8971 | 3.5351 | 6.9726 | +42% (noise) |
| 1x Zero-Latency | p50 | 0.1017 | 0.1085 | 0.0684 | 0.0689 | -36% |
| 1x Zero-Latency | p99 | 0.1247 | 0.1605 | 0.0910 | 0.0954 | -41% |
| 1x Zero-Latency | max | 0.3669 | 1.8085 | 0.2076 | 0.3810 | -79% |

Run-to-run variance before the change (3 earlier runs that were not interleaved): Eco p50
0.433-0.441, p99 0.718-0.782, max 3.4-4.8 ms; Render p99 2.36-2.72 ms.

Callgrind instruction count for `cd_bench 100 0`, which is deterministic and unaffected by load:
3.614e9 before, 2.188e9 after (-39%).

### Hotspots before the change (callgrind, share of instructions)

| Function | Share |
|---|---|
| MnaSolver::evaluate | 25.1% |
| MnaSolver::luSolve | 24.8% |
| MnaSolver::newton (self) | 11.5% |
| MnaSolver::finiteSolution | 8.1% |
| libm exp | 9.6% |
| PolyphaseHalfband2x::down / up | 4.6% / 4.1% |
| CircuitRuntime::process (self) | 3.3% |

A gprof (-pg) run gave the same ranking: luSolve 31%, evaluate 24%, newton 20%, FIR down/up 6% each.

### Changes. All are bit-identical: outputs were compared byte for byte against the old build.

Comparison set: the engine in Eco/Studio/Render/1x/8x-IIR at block sizes 1/37/128/129, ladder
netlists with m = 3..21 (both the specialised and the generic LU), and oversampler sweeps over
2x-16x, 100/150 dB and block sizes 1-512.

- The netlist is compiled into a flat stamp table at prepare. Ground indices are resolved and
  1/R, C/h, n*Vt and exp(cap) are cached. The element order and the floating-point operations
  are unchanged.
- Newton, evaluate and LU are specialised for m <= 8, with a runtime-size fallback. The LU checks
  finiteness once at the end instead of after every operation. The two are equivalent: once a
  value is non-finite, the elimination and back substitution never make it finite again.
- The Jacobian evaluated at the accepted damped trial becomes the next iteration's Jacobian. It is
  the same point in the same step context, so this is still a true Newton and not quasi-Newton.
  This removes one evaluation per non-final iteration.
- A diode exponential is reused when its argument is bit-identical. The first evaluation of each
  substep sits at the previous converged point.
- `finiteSolution` has an exact shortcut for |x| <= 2^500 and C <= 2^500. The redundant
  `finiteSolution` after a successful Newton was removed.
- FIR halfband: the block path computes four outputs with separate accumulators, each in the
  original summation order. Micro-benchmark: 4x up+down 13.7 -> 10.0 us and 16x 40.4 -> 30.2 us
  per 128 frames.

Not done:
- Halfband even-tap symmetry folding and multi-accumulator reductions were not used because they
  change the summation order.
- No compiler flags were added. `-march`/FMA contraction and `-ffast-math` would change results.
- ThermalNetwork is not a hotspot (well under 0.1%, 1 kHz) and was left unchanged.
