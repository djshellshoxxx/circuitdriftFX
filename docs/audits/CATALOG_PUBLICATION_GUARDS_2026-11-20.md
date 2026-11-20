# Preset catalog and scene publication guards — 2026-11-20 UTC

## Source and scope

Reviewed and repaired the state/library boundary on top of `c6ebb22daab9c46edffa761b21f3fd45320e5074`. This follows the preset service, atomic creation, checkpoint, writer roundtrip, and scene cardinality changes already present in that source. The changes affect worker/message-thread operations; they do not change audio processing, parameter IDs, schema versions, or sound capture/application.

## Catalog enumeration

`PresetLibrary::list` previously used the number of matching user results as its loop limit. A search with no matches, or a directory containing ignored entries, could therefore traverse the entire directory despite `kMaxListedPresets`. The live catalog calls this operation synchronously, making the difference between a result bound and an enumeration bound observable to the editor.

The loop now charges every directory entry before checking its extension, type, name, contents, or query match. Its optional `entryBudget` is capped at `kMaxListedPresets` (5000); factory entries do not consume it. Existing callers retain the same default arguments and return type. User results remain sorted by ID after filtering.

`PresetListReport` exposes the operation's scope:

| Member | Meaning |
| --- | --- |
| `examinedEntries` | Directory entries processed, including ignored entries and query misses |
| `readFiles` | Bounded-read attempts for candidate preset files, including unsuccessful attempts |
| `truncated` | More directory entries remained when the entry budget was exhausted |
| `error` | Directory enumeration failed; any returned user results may be partial |

A missing user directory is treated as an empty user library, consistent with creation on first save. Other directory enumeration failures are reported. Each call replaces the supplied report, including clearing stale diagnostics.

This bounds work counts. It does not establish a filesystem or UI response deadline. Truncated scans use filesystem enumeration order to choose the examined subset; sorting the returned subset does not establish a complete global ordering or pagination contract. The report allows the live caller to disclose an incomplete catalog and to select a smaller budget.

## Publication identity preservation

`saveState` already rejected repeated parameter identities in a directly constructed `StateDocument`. `publishScene` did not. `toJson` inserts parameters through `JsonValue::set`, so a repeated identity was reduced to its last value before the reader roundtrip check. The resulting document could be valid while having lost source data.

Both writers now share the same uniqueness predicate. Publication rejects repeated identities before serialization, whether the repeated values are equal or different. Failure leaves both the supplied output and the source parameter list untouched. Valid publication still uses the original serializer and intentionally strips UI, checkpoint, and unknown metadata fields.

## Focused validation

Configuration: GNU C++ 14.2.0, CMake 4.4, Release with `-O1 -g0 -DNDEBUG`, `CD_SANITIZE=ON`. Compiled flags include `-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer`. Build and CTest parallelism were both 1. Runtime used `ASAN_OPTIONS=detect_leaks=0` and `UBSAN_OPTIONS=halt_on_error=1`.

The regression was run before applying the publication production fix. `acceptance_core` and `state_preset` passed; `state_safety` failed exactly its two new duplicate-publication assertions. After the shared predicate was added:

| CTest suite | Result | Time |
| --- | --- | --- |
| `acceptance_core` | Passed | 2.37 s |
| `state_safety` | Passed | 0.16 s |
| `state_preset` | Passed | 1.51 s |
| Total | 3/3 passed | 4.06 s |

Catalog coverage uses three small synthetic presets and three ignored entries. It checks matching and zero-match queries under the same budget, exact-boundary completion, a zero budget, skipped entry accounting, missing and invalid directories, sorting, and report replacement. Publication coverage checks repeated IDs with equal and different values, output/source preservation, successful error clearing, and unchanged published bytes when additional private fields are present.

Reproduction after configuration:

```sh
cmake --build build --target cd_state_preset_tests cd_state_safety_tests cd_tests --parallel 1
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build --output-on-failure --parallel 1 \
  -R '^(state_preset|state_safety|acceptance_core)$'
```

Independent source review by the integration reviewer found no blocker in the catalog counting/report contract or the shared publication uniqueness check. The review confirmed that the live caller should surface `truncated` because filtering and sorting operate on the examined subset.

The run covers core state code on Linux. It does not qualify native editor behavior, Windows filesystem behavior, pluginval, the complete test set, or an integrated descendant head. Existing synthetic acceptance diagnostics retain their existing gates; this is not physical qualification or release approval.

## Remaining boundary

The writer roundtrip is still a schema check after serialization. For example, an otherwise valid directly assembled document can carry an opaque `ui` or metadata container whose nesting exceeds the reader's depth limit of 20. `toJson` recursively copies it and `core::writeJson` serializes it before `loadState` rejects it. An in-memory oversized opaque string similarly reaches copying/serialization before the document byte bound is checked. External JSON admission is already bounded by the reader; this review did not demonstrate an external path that bypasses it.

The smallest complete follow-up is a depth and encoded-byte preflight over the in-memory document before `toJson` copies the opaque trees, with publication checking only the fields it retains. A bounded serializer alone would leave the earlier recursive copies outside its limit. That separate admission change is not included in these two repairs.

Filesystem operations retain the documented trusted local-directory assumptions and existing bounded file reads and atomic no-replace installation. This patch does not expand those contracts.
