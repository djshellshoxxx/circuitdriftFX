# G12 local state blueprint cardinality and lossless writer guard — 2026-11-20 UTC

## Confirmed defect
`StateDocument::saveState` first checks that its *serialized* JSON can be read back. However, `toJson` used `blueprints[0]` for a scene without checking the number of source blueprints. A directly assembled scene containing two A/B blueprints therefore returned valid JSON while silently dropping the second blueprint. Likewise, a corrupted `DocKind` enum was mapped into a scene rather than rejected. The normal JSON parser could not expose this defect because it produces well-typed documents.

## Repair
- The local writer now checks scene cardinality (**exactly one** blueprint), session cardinality (**one or two**), and rejects invalid document-kind enum values *before* `toJson`.
- This preserves valid scene output and a supported two-blueprint session without changing the schema, JSON parser, real-time path, seeds, or engine API.
- `wholeDocumentWriterBoundary` regresses scene A/B data loss, missing scene blueprint, invalid enum, successful two-blueprint session roundtrip, and unsupported three-blueprint session.

## Evidence and independent-review handoff
Parent draft PR #24 exact head `b6a1acdf653590126416fd0229a94a40c3dd9117`, [CI run #44759057463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44759057463): Linux passed; Windows/pluginval remained **in progress** at source selection. Neither the parent result nor any earlier run qualifies this changed source.

Code review owner: check writer/reader cardinality symmetry and preserved two-slot session semantics. Bug-hunt owner: verify malformed `DocKind`, zero/three blueprints and unmodified canonical output. Defensive-security owner: examine in-memory payload loss before validation and malformed producer state. No separate workers could be started in the connector environment; there is no independent review signoff. No local native compilation was possible because the container cannot resolve GitHub. Exact-child-head Linux CTest, Windows/pluginval strictness 5 and independent reviews are required before guarded integration.

This is an incremental G12 *loss-prevention* repair, not live runtime/control/seed capture, audio-safe checkpoint handoff or complete host session continuity. PR #16 and all parent heads and branches remain unchanged. No release, deployment, force write or branch deletion.
