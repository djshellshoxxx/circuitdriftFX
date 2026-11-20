# Engine checkpoint maintenance — 2026-11-19

## Provenance and scope

The initial bug-hunt workers were interrupted by automatic content screening. Their
partial work was preserved; they are not counted as completed audits. The primary
worker reviewed and completed the bounded engine/DSP changes in this checkpoint.
This is a source and synthetic-unit-test maintenance pass, not a release qualification.

## Changes

- Validate FIR positions in floating point before integer conversion, and validate
  every oversampling stage before mutating any stage.
- Give nested solver, thermal, and gain-match state setters explicit success results
  and matching non-mutating validators. Inductor validation checks the candidate
  stored charge/flux rather than the current object's stored value.
- Add a non-mutating checkpoint preflight for lengths, layout, sample index, integer
  selectors, booleans, delay positions, transitions, RNG state, and nested DSP state.
- Allocate a rollback checkpoint during preparation. Restore pre-existing thermal
  configuration before replaying a rollback image if a nested numerical setter fails.
- Preserve the existing checkpoint layout; this change tightens accepted values.

## Evidence

GCC 14.2.0, Release with `-O0 -DNDEBUG -g0`, `CD_SANITIZE=ON`, serial build.
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` is required in this
container because LeakSanitizer cannot run under its tracer. CI configuration is unchanged.

- `engine_runtime` and `engine_checkpoint`: **2/2 passed**, 282.95 seconds in total,
  before the final rollback ordering/comment adjustment. This includes normal
  checkpoint continuation and rejection without changing saved state.
- `oversampling_safety`: **passed**, 0.93 seconds, including new finite out-of-range
  position checks and unchanged state when a later cascade stage rejects its input.
- The final integrated candidate must rerun these affected targets and the full core
  suite. Windows execution is a separate CI gate.

## Limits for the next reviewer

The checkpoint API is an internal prepared-engine contract. These changes validate
its structural and numerical state; they do not establish a physical plausibility
proof for every finite filter coefficient or electrical value. Persisting a complete
host session still needs a versioned runtime/seed/edition migration contract. The
plugin's custom editor and service bridges are also separate unfinished work.
