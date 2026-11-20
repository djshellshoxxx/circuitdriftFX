# State writer resource admission — R14, 2026-11-20

## Repair and scope

`saveState` and `publishScene` now count retained JSON depth and encoded bytes
before `toJson` copies opaque values. The previous whole-document reader
roundtrip remains the semantic acceptance check, but it is no longer the first
resource check. This is a worker/message-thread state API; audio code is unchanged.

The admission pass matches the existing writer's quotes, escaping, separators
and number formatting. It checks the reader's 20-container nesting limit, the
1 MiB blueprint bound, the scene/session envelope limits, and the metadata
budget of raw key bytes plus encoded value bytes. It stops at a limit without
building the output string or copying an opaque tree. Checkpoint JSON is built
only after the existing bounded typed-field checks.

`JsonValue` default copies include inactive storage. Retained values with
nonempty string/array/object members excluded by their type tag are therefore
rejected before copying. Canonical parsed values use only their active storage.
Publication excludes private UI, checkpoint and metadata from admission because
those fields are not copied or emitted. A failed publication leaves the caller's
output unchanged; successful valid documents retain their serialized bytes.

## Independent source review

The separate integration reviewer inspected the complete two-file implementation
and test diff based on state-worker commit
`11cb8399296584ba6b018eb2077c7bb14392e5fa`. Reviewed blobs:

- `src/state/StateDocument.cpp`: `68c62945b954fa012499d88ce2b8ad56206c4fe1`
- `tests/test_state_safety.cpp`: `8bb0e04c3064ddf88634bbcc20b30ebbae737ef7`

The coordinator independently compared the counter to `toJson`, `writeJson`,
parser depth handling and the publication field policy. No blocking discrepancy
was found. This review did not execute inputs or establish test results.

The helper intentionally mirrors the fixed version-one envelope. Changes to
the envelope or JSON encoding require corresponding accounting changes. The
reader roundtrip and byte-equality regressions protect this maintenance boundary.
Encoded-byte limits are not a claim that total in-memory allocation equals the
encoded size.

## Regression scope and execution ownership

The ordinary unit fixtures exercise exact schema depth/byte limits and one-step
rejection, escaped strings/keys, both scene and two-slot session envelopes,
byte-preserving valid output, inactive retained storage, and private fields
excluded from publication. Fixture depth stays at the schema boundary; this is
validation of the repaired serializer, not a crash or stress test.

The coordinator owns compilation and combined sanitizer/native execution of
the integrated source. Read `docs/engineering/VALIDATION_2026-11-20_R14.md` for
the resulting source hashes, commands and evidence. This source-review note
alone makes no native, Windows, pluginval or dynamic-session qualification claim.
