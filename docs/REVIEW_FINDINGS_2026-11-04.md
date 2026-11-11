# Review findings ledger — 2026-11-04 (PR #9)

AI-assisted independent review of the modules merged on PR #9 (control, state queues/A-B/history, StateDocument repairs, blueprint, community). Not a professional audit. Each finding was reproduced (ASan/TSAN/repro program) or traced before fixing; regressions live in `tests/test_review_regressions.cpp` unless noted. Fixed in commit f3b7cd1.

| ID | Sev | Location | Defect | Fix / evidence |
|---|---|---|---|---|
| C1 | Critical | src/state/TransactionQueue.cpp `place` | Param/Config appends ignored the 4096 append capacity after reserved note-offs → heap overflow inside noexcept `drainSlice` (audio thread) on stalled audio + MIDI panic + automation | Coalesce once full; ASan clean; ≤4096/slice; 2048 note-offs survive or AllNotesOff forced |
| H1 | High | src/core/Json.h | clang+libstdc++ could not compile JsonValue (recursive `pair<string,JsonValue>` instantiation via implicit default ctor); macOS toolchains likely affected | Out-of-class defaulted noexcept default ctor; clang Release build + tests pass; CI `linux-clang` job added |
| M1 | Medium | src/state/History.cpp | Undo/redo restored whole documents, reverting host-automated parameters outside the transaction | Restore only `affectedIds` when live doc drifted (`restoreTarget`); T09 unchanged |
| M2 | Medium | src/community/CatalogClient.cpp | Unsynchronised RNG across 2 concurrent requests (TSAN race) | Single `rngMutex_`; TSAN clean |
| M3 | Medium | src/blueprint/Faults.*, src/dsp/MnaSolver.h | `long` sample counters (32-bit on Windows x64) overflow after ~3 h at 192 kHz electrical rate → frozen intermittent fault | `std::int64_t`; static_assert (not reproducible on LP64 Linux). Runtime diagnostic `long` counters still to convert |
| L1 | Low | tests/test_blueprint_timeline.cpp | `memcmp` over padded struct; fails under clang | Field-wise comparison |
| L2 | Low | src/control/Macros.cpp | Unvalidated manifest bindings → NaN/UB (`std::clamp` precondition) on audio evaluator; tolerance not capped at ±25% | Bindings validated; invalid → macro unsupported; cap min(maxTolSigma,0.25) |
| L3 | Low (latent) | src/blueprint/Compiler.cpp | `nodeOf[2]` indexed by pin count → stack overflow for ≥3-pin models | Reject non-2-pin models with UnsupportedModel |
| L4 | Low | src/community/CatalogClient.cpp | Logs contained presigned URL query strings | Strip from `?`/`#` |
| L5 | Low | src/community/CatalogClient.cpp | Unbounded list/revision caches | 64-entry LRU |
| L6 | Low | src/community/CatalogClient.cpp | Throwing transport leaked an in-flight slot → later calls spin | RAII release guard |
| L7 | Low | src/control/PitchDetector.cpp | Rejected-jump branch skipped 200 ms hold expiry → stale pitch held indefinitely | Expiry applied; chirp regression |

Also fixed: RtTrap positive control in `test_state_queue` was elided by clang Release (kept alive). Checked with no defect: RtQueue SPSC, GenerationPool reclaim, SnapshotExchange, AbMixer, SHA-256 (vs hashlib 0–299 B chunked), LocalCache, StateDocument repairs, quantizer/tempo/delay, blueprint parser bounds.

Residual: clang build ran without sanitizers (runtime missing locally); libc++ not tested; runtime `long` diagnostic counters pending (owned by the engine-wiring writer).

## Round 2 — 2026-11-11 (network, media, edition, uimodel, analysis, runtime additions, CI)

Same method (AI-assisted, read-only reviewer, repro per finding; not a professional audit). gcc `-O2 -D_GLIBCXX_ASSERTIONS` 43/43 pass; TSAN 19/19 suites clean (cross-process network traffic invisible to TSAN). No Critical/High.

| ID | Sev | Location | Defect | Status |
|---|---|---|---|---|
| R2-M1 | Medium | .github/workflows/nightly.yml | Stress/benchmark steps could never fail (`cmd \| tee` status, PIPESTATUS read after a later pipeline) | **Fixed**: capture `rc=${PIPESTATUS[0]}` immediately, `exit $rc`; benchmark `shell: bash` (pipefail). Simulated failing binary → step exit 3 |
| R2-M2 | Medium | src/network/Member.cpp `attach` + Protocol.cpp `validateRegion` | Double fetch of shared header: second copy only partly re-validated → same-uid writer can inject NaN sampleRate / negative rail (UB in int64(ceil(NaN))) | **Unfixed** — use the validated header copy only (return it from validateRegion) |
| R2-M3 | Medium (test harness) | tests/support/RtTrapWrap.cpp, CMakeLists wrap list | Trap misses pthread_cond_clockwait, pthread_mutex_clocklock, pwrite/fsync/nanosleep/writev, and calls from inside shared libs → false passes possible | **Unfixed** — extend wrap list; document LD_PRELOAD for in-library calls |
| R2-L1 | Low | tests/support/RtTrapWrap.cpp | `flags & O_TMPFILE` true for any O_DIRECTORY open → va_arg UB | **Fixed** (glibc test `(flags & __O_TMPFILE) == __O_TMPFILE`) |
| R2-L2 | Low | src/runtime/Engine.cpp `readState` (CDE1 v2) | Macro/filter state, bools and Scenario enum copied raw from snapshot payload with spot checks only; 1e300 state accepted (output still contained) | **Unfixed** — validate every double, route timeline via restore(), store enums/bools as checked integers. No external payload source wired today |
| R2-L3 | Low | src/media/WavWriter.cpp | Data cap ignored header bytes → RIFF size field could wrap near 4 GiB | **Fixed** (cap = 0xFFFFFFFF − header − 1) |
| R2-L4 | Low | src/edition/LiteState.cpp | Seed compared as double → 2^53+1 accepted and silently saved as 2^53 | **Fixed** + regression in `edition_lite` |
| R2-L5 | Low | .github/workflows/*.yml | Actions pinned to mutable tags (@v4) | **Unfixed** — pin to full commit SHAs (supply-chain hardening) |

Checked, no defect: shm create/open (O_EXCL, 0600, fstat owner/mode/size, O_NOFOLLOW), ring index/masking, broker/co-render bundle bounds, FileSink O_EXCL temp, PNG/GIF/LZW bounds (re-verified vs Pillow 12.3), LITE JSON limits, uimodel parsing, telemetry clamps, SlotEngine/ProcessorGraph, setElementParameter. Residual untested: shrinking ftruncate of the shm object by a same-uid process (SIGBUS).
