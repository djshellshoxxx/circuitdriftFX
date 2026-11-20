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

## Follow-up: queue ordering failure from combined qualification

The coordinator's later combined core run passed 29/30 suites and exposed a
failure of the existing `state_queue` monotonic assertion. Its queue diagnostics
reported 6,459 ring pushes and 9,541 coalesced pushes. The earlier five-suite pass
above remains evidence for that particular execution; it did not establish that
all queue schedules were correct. The failing assertion was retained unchanged.

### Cause and repair

The consumer snapshots the ring tail to bound each drain. After it releases the
consumed ring slots, the producer can append new ring events and publish an even
newer coalesced value before that drain's coalescing phase. The consumer can thus
apply the newest value and encounter an older ring event on its next drain.
The previous separate value/gesture atomics also did not transfer one coherent
publication after the pending flag was cleared.

Source commit `43d2d777670e068c6be3d6aa7724a7afca403035` replaces each
coalescing slot with a three-slot SPSC mailbox and gives every accepted event a
per-parameter sequence number. The consumer remembers the newest sequence it
has applied and skips older ring events already superseded by a coalesced value.
Numerical decreases remain valid; freshness is decided by sequence, not value.
Sequence exhaustion rejects further pushes with an explicit false return and
rejection count instead of wrapping.

Mailbox ownership remains disjoint: the producer owns its back slot, the
consumer owns its front slot, and an atomic middle slot transfers ownership.
An acquire/release exchange publishes a fully initialized event and returns a
slot whose prior owner has finished using it. The producer cannot overwrite the
consumer's active slot. The complete value, gesture identifier and sequence
therefore travel together. Coalesced events still use sample offset zero and no
gesture kind, as in the existing overflow contract.

The callback retains its fixed bound of at most the ring capacity plus the
parameter count. It introduces no allocation, lock, wait, retry loop, or I/O.
Lock-free compile-time checks cover the mailbox index, ring sequence atomics,
queue ownership guards, and the separate snapshot publisher's atomic doubles.

### Regressions and independent review

- A deterministic test pauses between the actual production ring and coalesced
  drain phases. It appends an older ring update and a newer coalesced update,
  then verifies that the next drain cannot replay the older value or event
  record. A subsequent genuine value decrease is still delivered.
- A focused concurrent overflow test checks that every observed value matches
  its gesture identifier, records remain ordered, callback work is bounded, and
  the final accepted value is delivered. Overflow is guaranteed before the
  consumer starts.
- A bounded counter-boundary test verifies explicit exhaustion rejection.
- Existing overflow counts, monotonicity, final-value delivery, ownership,
  no-allocation, `noexcept`, and coherent snapshot assertions remain enabled.

A separate read-only reviewer checked the mailbox's C++ ownership/memory-order
argument, stale-event suppression, and deterministic test. No blocking finding
remained. The review's request to retain the atomic-double lock-free assertion
was resolved by placing it with `ParamSnapshotPublisher`, which still uses it.

### Follow-up validation

The exact source and tests in `43d2d77` passed **25 fixed repetitions** of
`state_queue` in **4.52 seconds** using GCC 14.2, C++20, `-O1 -g -DNDEBUG`,
ASan, UBSan, and float-cast-overflow instrumentation. The command stopped on the
first failure; all 25 passed. No assertion or acceptance threshold was reduced.

```sh
cmake -S . -B ../cdfx-queue-build -DCD_SANITIZE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O1 -g -DNDEBUG' -DCD_BUILD_PLUGIN=OFF
cmake --build ../cdfx-queue-build --target cd_state_queue_tests -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir ../cdfx-queue-build --output-on-failure -R '^state_queue$' --repeat until-fail:25
```

This is focused verification of the queue repair, not a replacement for the
coordinator's full combined run. No ThreadSanitizer, Windows, native plugin,
physical-model, or release-performance result is claimed by this follow-up.
