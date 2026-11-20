# Session checkpoint exact-head CI and in-memory bounds follow-up — 2026-11-20 UTC

## Observed exact-source failure

Draft PR #21 at `0487a63d07e58a228a32705f03272efacc8fdba9` failed [CI run #44731856158](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44731856158): Linux `acceptance_core` failed at `tests/test_main.cpp:515` (29/30; 29 passed). The persisted synthetic checkpoint carried two RNG words, but the fixture's six-field prepared `RuntimeIdentity` defaulted `rngSize` to zero. The new production gate correctly rejected this incompatible shape. `state_safety` passed; Windows was skipped. PR #20's distinct exact-head Linux and Windows/pluginval checks passed. No existing test or threshold is disabled.

## Repairs on isolated child

- Declare the two RNG words in the synthetic prepared identity and add a negative mismatch assertion to the original acceptance suite.
- Reject *programmatic/in-memory* checkpoints exceeding the existing schema-v1 parser bounds: 4096 electrical, 4096 thermal, 64 RNG words, even if their alleged `RuntimeIdentity` shape matches. Return `ResetWarm`, not partial dynamic state.
- Regress all three oversize shapes in `state_safety`, asserting the reason and success after reverting to a compatible layout.

## Verification and handoff

Exact-child-head native CI, Windows pluginval strictness 5 and DAW/host qualification remain required. Source substitutions and JSON validation were checked; no local native run was possible because the container cannot resolve GitHub. Independent code/bug/security workers are unavailable through this connector; **no signoff is claimed**.

Code review: verify each real prepared `RuntimeIdentity` supplies engine-owned RNG count rather than copying the checkpoint's untrusted count. Bug hunt: check boundary lengths, valid restored values, and no stale memory after ResetWarm. Defensive security: examine programmatic checkpoint construction and stopped-audio failure-atomic installation. Full host dynamic/seed/session continuity is not implemented by this change. No main modification, force update, release or deployment.
