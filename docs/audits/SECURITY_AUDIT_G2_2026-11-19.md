# Security audit handoff — cache persistence and influence bundles

## Source and scope

- Reviewed base: PR #15, `0c0d6897caf36d4057ac3d8db8cfb346dd5412b4`.
- Continued the first worker's pending regression fixtures; production fixes were completed by the fresh successor.
- Cache commit: `c31b7f4d9b5515e255a0d6ebdfcff2a4ca96dff4`.
- Bundle commit: `0dbe74300560ce653df7efacecbb020a6eadd9bd`.
- Changed only `src/community/OfflineCache.*`, `src/network/InfluenceBundle.*` and their existing test suites, plus this handoff.
- Root owns runtime integration, blueprint recovery, CI and the state-queue scheduling test. Other workers own engine/DSP and plugin/UI. No remote writes, main merges, releases or configuration changes were performed by this worker.

## Completed repairs

### Cache persistence

The old cache used truncating stream writes for predictable temporary files and pin markers, followed entry links during directory scans, and reported successful pinning even when persistence failed. It also deleted unrelated files ending in `.part`. New local fixtures reproduced six failed checks before the source repair.

The cache now reuses `state::saveStateFileAtomic`, whose exclusive temporary-file creation, flush/close checks and atomic replacement already implement the shared POSIX/Windows persistence contract. Paths are passed as UTF-8, including Windows. Entry scans/read checks use `symlink_status`; marker and destination links are replaced instead of followed during writes. A scan no longer deletes unknown temporary files. Pin state changes only after its persistence operation succeeds.

Pinned capacity is checked before replacement. A too-large update to a pinned entry leaves its previous bytes and accounting unchanged. Tests also verify that failed pin writes do not change subsequent eviction policy, and that UTF-8 cache directories work locally.

The cache directory must remain trusted and must not be concurrently modified by another process. This pass verifies stable filesystem fixtures and the existing shared atomic-write contract; it does not claim protection against concurrent directory substitution or establish cross-process cache locking.

### Influence-bundle validation

The parser verified its non-cryptographic integrity hash and broad dimensions but accepted non-finite payload floats and inconsistent semantic metadata. Direct in-memory replay and serialization had different validation rules. New regression fixtures reproduced 32 failed checks before the source repair.

One validation path now enforces the supported version/sample-rate range, member/hash count agreement, sorted unique members, nonwrapping sample ranges, bounded model-version strings, finite metadata, nonnegative source resistance and the documented bleed-gain cap. Payload-size arithmetic checks the 256 MiB cap by division before multiplying lengths. Tick calculation no longer wraps when rounding a large length upward.

Parsing, serialization, replay and co-render all validate payload dimensions and finite samples. Serialization returns an empty byte vector for invalid in-memory bundles. Failed parse/co-render leaves prior output unchanged; failed replay open clears its previous reference. Co-render rejects non-finite stems/topology/tolerance and values that cannot fit in the float payload before conversion.

The proposed 48-quantum offline-delay restriction was rejected: that limit belongs to the live ring implementation, not the offline bundle contract. A 49-quantum co-render/serialization/replay case remains accepted and tested.

FNV-1a remains an accidental-corruption/content-mismatch check, not authentication. `BundleReplayer` borrows its bundle; the caller must keep that object alive and immutable throughout replay. These contracts are explicit in source comments.

## Verification

Toolchain: GCC 14.2, CMake 4.4.5, C++20, Debug `-O0 -g0`, ASan + UBSan + float-cast-overflow. Build concurrency was limited to one job because the shared execution environment had memory pressure.

```sh
cmake -S . -B build-security-g2 \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS_DEBUG='-O0 -g0' -DCD_SANITIZE=ON
cmake --build build-security-g2 --target cd_community_tests cd_network_tests --parallel 1
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-security-g2 \
  -R '^cd_(community|network)_tests$' --output-on-failure --parallel 1
```

Result after both fixes and final regression additions: **2/2 targeted suites passed**, 15.53 seconds total. `git diff --check` passed. The network suite retains its existing concurrent publish/consume and deterministic replay tests; its timing output on this shared host is informational, not a performance qualification.

A separate run without the local `detect_leaks=0` override failed at LeakSanitizer shutdown in both executables: `LeakSanitizer does not work under ptrace`. Leak detection therefore remains unqualified in this environment. No CI sanitizer setting was changed. Windows execution, host/plugin integration, physical-model validation and a whole-repository audit were not claimed by this worker.

## Fresh-worker continuation

1. Independently review the two commits, then run the community/network suites on the integrated candidate and Windows CI. Keep the default CI sanitizers enabled.
2. Reproduce and repair the remaining cache accounting paths around failed removal: `OfflineCache::evictLocked` and `remove` currently discard bookkeeping even when `filesystem::remove` reports an error. Also check reopening an existing directory whose persisted pins exceed a newly configured capacity; `open` currently returns success after `evictLocked` cannot reduce pinned content. These are source-review follow-ups, not outcomes exercised in this pass.
3. Continue reviewing the shared-memory envelope and broker attachment/lifetime contracts in `InstanceNetwork` and `SharedMemory`, with local temporary mappings only. Preserve the documented G05 host/process qualification gate.
4. Check the plugin/runtime callers' ownership of borrowed replay bundles and cache directories once those services are connected. Do not infer end-to-end integration from these standalone suites.

