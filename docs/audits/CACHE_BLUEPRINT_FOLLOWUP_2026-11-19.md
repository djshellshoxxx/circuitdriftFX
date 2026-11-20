# Cache and recovered blueprint follow-up — 2026-11-19

The security G3 worker was interrupted by automatic content screening after
leaving bounded cache regression fixtures. The coordinator preserved those files
and completed this maintenance pass. Attempts to start an additional worker hit
the session thread limit; this is not presented as a fourth independent audit.

## Confirmed repairs

- Cache deletion failures no longer erase bookkeeping for data still on disk.
  Corrupt-entry cleanup preserves accounting if deletion fails, and an eviction
  pass tries each eligible entry once before returning a capacity error.
- Cache writes make room before replacing the destination. A failed eviction
  cannot leave an unaccounted new file or discard the previous destination.
- Reopening a cache checks the total persisted pin reservation before eviction,
  refuses capacity below that reservation, and leaves pinned files intact.
- The direct typed blueprint compiler now reapplies schema validation, with
  resource preflight and duplicate-key checks before canonicalisation. Invalid
  modulation indices, duplicate bindings/keys, unsupported schema and nonfinite
  values are rejected before graph creation.
- Recovered blueprint code depended on a separate generated-source allowance
  absent from the newer MNA solver. The solver now distinguishes at most 64 user
  components from at most four explicitly marked internal port sources. A device
  cannot use that allowance. The compiler marks its own injected input source.

## Verification

GCC 14.2, `-O0 -DNDEBUG -g0`, serial CMake build, ASan/UBSan/float-cast-overflow.
Local `ASAN_OPTIONS=detect_leaks=0` addresses the container's ptrace limitation;
the repository CI sanitizer configuration was not relaxed.

All four suites passed in 36.84 seconds: `cd_community_tests`,
`blueprint_compile`, `blueprint_timeline`, `blueprint_faults`. This includes the
recovered original suites and new typed-input/cache-failure regressions. One
additional port-budget direct-solver boundary was then added; the compile suite
is rerun for that change. The integrated coordinator build must be refreshed.

## Limits

The cache requires a trusted directory without concurrent external modification;
these static checks do not establish a race-free multi-process filesystem API.
Evictions are disposable cache operations and are not rolled back if a later
file write fails. Complete session persistence and installing a compiled custom
blueprint into the live host runtime remain separate work. All models and probe
evidence here are synthetic; native platform execution remains a CI gate.
