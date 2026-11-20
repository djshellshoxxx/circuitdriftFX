# R15 scoped source review — ConfigDelta and History conflict-safe replay

**Base:** PR #16 R14 `02a7331cc687e710f3a3c7c0ab023e651601685e`  
**Branch:** `codex/cdfx-history-conflict-20261120-r15`  
**Scope:** portable message/worker-thread configuration transactions only.

## Concrete defect and correction

Before this patch, `History::undo` and `redo` moved the stack entry and blindly applied a delta.
A later host change to the same parameter could be clobbered. Moreover, a blueprint-only
slot-meta delta replayed its unchanged seed (and vice versa), erasing an unrelated
metadata edit. The old `applyForward/Reverse(Config&)` were `noexcept` despite assigning
`std::string` metadata, which can allocate.

Changed semantics:

- `canReplayDelta` checks the current *touched* source fields against the recorded
  before/after side, including duplicate IDs, valid parameter ranges and discrete
  values, blueprint identifiers and seed limits. Unrelated fields are not conflicts.
- Config replay only assigns metadata fields that changed in the recorded delta.
- History prepares its candidate configuration before modifying either deque and
  then swaps the candidate into place; an allocation failure before deque mutation
  leaves the original config and history stacks intact.
- Config-delta metadata writes may now propagate allocation failure to the
  non-audio caller instead of terminating from a `noexcept` boundary.

Synthetic `state_history` regressions cover unrelated automation survival,
same-field undo and redo conflicts, idempotent retry, blueprint/seed field
independence, stale meta rejection, and malformed/duplicate/oversized identities.

## Qualification and defensive review limits

The implementation and regressions were inspected at source level. **No local native
compiler or test execution was available through the connector-backed checkout**.
Use exact-child-head Linux sanitized core tests and Windows/pluginval, then an
independent code reviewer, functional bug hunter and defensive security reviewer
before importing to PR #16. A GitHub Actions pass is evidence for its own head only.

Review risk: non-atomic APVTS multi-parameter host publication remains separate.
The portable History class is message-thread only; it does not solve concurrent
same-parameter audio/host state transactions. It intentionally does not change
wheel-group admission, dynamic checkpoint bytes, real models, GIF or services.
No releases, deployments, main updates or original branch rewrites are included.
