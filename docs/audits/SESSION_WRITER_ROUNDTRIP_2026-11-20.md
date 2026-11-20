# Session writer whole-document reader symmetry — 2026-11-20 UTC

## Confirmed issue and repair

The G12 checkpoint preflight in PR #23 bounds the optional dynamic state but does not ensure directly assembled StateDocument envelopes remain loadable. Previously `saveState` could serialize unknown feature IDs, invalid blueprints, out-of-range parameters, invalid dates and metadata above the reader's 64 KiB limit; its `toJson` path silently replaced incorrectly typed macro/timeline/fault containers with empty containers and let unknown metadata override reserved keys. This permitted unreadable local state or silent loss of data.

The new worker/UI-thread writer validates container types and rejects duplicate parameter identities, reserved metadata names and duplicate metadata keys before generating JSON. It then re-loads the finished text through the production schema-v1 reader, rejecting invalid envelopes with `std::invalid_argument` instead of returning a document the reader cannot accept. This reuses exact reader limits rather than duplicating their constants. No audio-thread changes are made.

## Regression coverage and boundaries

`tests/test_state_safety.cpp` now checks malformed dates, engine versions, titles, feature IDs, duplicate/unknown/nonfinite parameters, reserved/duplicate/oversized metadata, unknown synthetic fixture models, invalid containers and UI types, and a byte-for-byte unchanged valid roundtrip. Existing scene publication strips local fields and keeps its independent `publishScene` validation; versioned checkpoint restore compatibility still belongs to `decideRestore`.

Parent [PR #23](https://github.com/djshellshoxxx/circuitdriftFX/pull/23) at `b9994290a8db03b50535f597ab76649e4b347b2f` passed [exact-head run #44749975047](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44749975047), Linux and Windows VST3/pluginval. That success does **not** qualify this child. Child source changed after the passing run; Linux state_safety/sanitizers and Windows/pluginval are required before guarded integration. No local Git clone/native JUCE toolchain could be obtained (container DNS to github.com unavailable). Separate code, bug and defensive security workers were not exposed in the connector runtime; this note is a scoped author review, not independent signoff.

Still open: actual runtime/control/seed capture, safe stopped-audio apply, host session linkage, dynamic A/B transactions, older PR #9/#10/#14 semantic reconciliation, real-host measurements and physical model evidence. No release, deployment or alteration of parent branches authorized.
