# State and cache review: continuation handoff

## Scope and source

This independent worker reviewed the state history, preset serialization/library,
parameter publication, and community cache paths from the isolated editor branch
at `3f122df`. The review is defensive source maintenance with bounded synthetic
regressions. It does not qualify live services, native hosts, Windows file APIs,
physical circuit models, or the entire repository.

The source work is isolated on `codex/cdfx-state-review-20261120`. No original
development branch was replaced, no PR was edited, and no remote ref was moved
by this worker.

## Repairs

### Transactional wheel history

`History::commitWheel` previously changed the retained transaction directly when
coalescing wheel edits. Unlike a new transaction, a growing wheel group did not
pass the configured byte budget. Rejection could not preserve the old transaction
because there was no admission check before mutation.

The replacement group is now built separately, accounted, and admitted before
changing history. A rejected group preserves its prior delta, timestamp, byte
count, and undo behavior. A new edit after undo creates a new transaction rather
than merging into a preceding group. Changes made by the host to values touched
by the preceding group also end that group, so undo of the new user edit restores
the intervening host value. Non-finite grouping timestamps are rejected.

The regression suite verifies the exact bounded-history rejection, undo/redo
after rejection, new edit after undo, and an intervening host automation value.
Commit: `5e2bfa534c2cc25e4de07c14ecf0511360591180`.

### Bounded preset reads

`PresetLibrary` checked file size before reading, but then consumed the stream
to EOF. The read now allocates only the checked size and reads that exact amount.
A short read or extra byte rejects the operation before publishing the result.
This retains the byte limit when a file changes during a read.

Regression coverage accepts a valid scene padded exactly to the 1.25 MiB limit,
then rejects the same file with one additional byte while preserving the saved
identifier and library entries. Existing import/export and invalid-file checks
remain enabled. The bounded-read guarantee follows from the exact-size read;
this test does not claim deterministic concurrent-filesystem race coverage.
Commit: `b1f0a0f`.

### Coherent parameter publication

`ParamSnapshotPublisher::read` previously wrote the candidate values directly
into the caller's output before checking coherence. A failed attempt could
therefore overwrite the last accepted UI snapshot despite returning zero.

The reader now copies into a local fixed-size candidate and publishes it only
after the sequence check passes. A failed read leaves the output unchanged.
The existing single-writer concurrent regression checks this invariant on every
observed failed read, as well as coherent successful reads, monotonic generation
numbers, and allocation-free operation. No extra audio-writer work was added.
Commit: `87fc6d0`.

## Verification

GCC 14.2, C++20 Debug, AddressSanitizer, UndefinedBehaviorSanitizer, and
float-cast-overflow instrumentation were enabled. Five scoped suites passed:

| Suite | Result | Seconds |
| --- | --- | ---: |
| `state_safety` | Passed | 2.92 |
| `state_queue` | Passed | 5.39 |
| `state_history` | Passed | 0.07 |
| `state_preset` | Passed | 17.66 |
| `cd_community_tests` | Passed | 1.25 |

CTest reported 5/5 passed in 27.40 seconds. These results cover the combined
production and test changes above. The last subsequent header edit only wrapped
a comment. `git diff --check` passed.

Commands used (the available CMake/CTest executables were under the coordinator's
`build-tools/cmake/data/bin` directory):

```sh
cmake -S . -B ../cdfx-state-build -DCD_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug -DCD_BUILD_PLUGIN=OFF
cmake --build ../cdfx-state-build --target cd_state_queue_tests cd_state_history_tests cd_state_preset_tests cd_state_safety_tests cd_community_tests -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir ../cdfx-state-build --output-on-failure -R '^(state_queue|state_history|state_preset|state_safety|cd_community_tests)$'
```

Leak detection was disabled for this managed environment; no leak-check result
is claimed. Native Windows, JUCE integration, the final combined candidate, and
CI still require their own exact-source qualification.

## Older-branch reconciliation

The following source comparison used PR #10 at `71a2637`, the retained PR #9 line,
and PR #14 at `ed81a92`. It identifies overlap and remaining work; it does not
claim complete semantic equivalence or authorization to close those PRs.

| Earlier implementation | Current implementation | Disposition |
| --- | --- | --- |
| `state/AbSlots` | `Config`, `ConfigDelta`, `ABOperations` | Configuration copy/swap, per-slot metadata and mirrored crossfade already exist. Dynamic engine state must still be coordinated by the adapter. No duplicate module imported. |
| JSON-blob `state/History` | Configuration-delta `state/History` | Twenty-step history and accounting are present under a different contract. Current wheel grouping was repaired. Old whole-document restoration is not a drop-in replacement for configuration deltas. |
| `state/SnapshotExchange` | Parameter publication plus engine checkpoint primitives | Old provider-layout identity and stopped-audio snapshot exchange remain useful design inputs. A complete runtime/control snapshot format and handoff still need definition; copying the old container would not establish exact continuation. |
| `edition/LiteState` and `LiteEngine` | Plugin edition markers, current LITE mapping/runtime | Older LITE upgrade export and full-scene compatibility analysis are not recovered here. They depend on retired prepared-engine types and model assumptions; an explicit adaptation is still required. |
| In-memory `community/LocalCache` | Persisted `community/OfflineCache` | Current disk cache has bounded entries, digest checks, LRU eviction and persisted pins. Its immutable ID/revision keys differ from the old digest-key API. No redundant cache was imported. |
| PR #14 `control/ControlHub` | Parameter queue plus current runtime/control APIs | Generation-tagged full-configuration and checkpoint handoffs require coordination with the runtime owner. Their presence in an old standalone hub does not qualify the current plugin. |

## Cache findings and remaining boundaries

The earlier pin-cleanup repair is present: forgetting a missing or corrupt entry
also attempts to remove its persisted pin. Its existing community regressions
pass in this run. Existing destination writes use the common atomic-file helper;
cache failures and pin/capacity behavior were reviewed without replacing that
implementation.

The directory remains a trusted, single-process cache boundary. Concurrent
external filesystem changes and failed pin-marker deletion are not made atomic
by this review. Preset `overwrite=false` still assumes serialized writers; the
existence check and later replacement are separate operations. A future
multi-process library contract should use explicit exclusive-create semantics.

Configuration session JSON already preserves all registry parameters and both
slots' blueprint identifiers/seeds, but deliberately rejects UI data, dynamic
checkpoints, and unsupported opaque graphs. Full plugin session continuity,
LITE upgrade interoperability, and integration with real preset/blueprint
services remain separate work. Do not mark G12 complete from these repairs.

## Next owner

Integrate the three source commits with the other workers' reviewed changes,
then run the combined state/runtime/plugin gates. Keep the older branches until
their remaining snapshot and edition features have an explicit disposition.
