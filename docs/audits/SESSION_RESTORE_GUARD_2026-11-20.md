# Session checkpoint restore compatibility — 2026-11-20 UTC

## Confirmed omission
`state::decideRestore` compared engine/revision/Fs/OS plus electrical and thermal vector sizes
but omitted RNG vector size. This could return `Restore` for incompatible prepared
stochastic state. Programmatic in-memory checkpoints also lacked a finite-value
and sampleIndex range check at this decision boundary.

## Reviewable child fix
From PR #20 at `8deb187da5cb8e0428b61d50b4a19be7307e6385`, isolated on `codex/cdfx-session-checkpoint-guard-20261120`:
- Extend `RuntimeIdentity` with the prepared expected `rngSize`.
- Fail closed on any electrical/thermal/RNG size mismatch, invalid sample
  rate/oversampling, nonfinite electrical/thermal state, or v1 sampleIndex overflow.
- Preserve existing `ResetWarm` fallback and error reasons.
- Extend `tests/test_state_safety.cpp` for matching RNG, mismatch, malformed
  in-memory state, overflow and no checkpoint.

## Limits and next owners
Source and JSON assertions only; no local repository checkout or native JUCE build.
The child has no exact-head CI evidence yet. Independent code, bug-hunt and
defensive security workers were unavailable. Code reviewer: verify all
`RuntimeIdentity` producers supply exact expected RNG size. Bug hunter:
exercise layout/finite/boundary cases. Security reviewer: audit forged in-memory
checkpoint admission and failure-atomic engine installation. Next engineering
work is a coordinated audio-owner dynamic checkpoint, stopped-audio restore
and versioned host serialization, not just this compatibility preflight.
No release, deployment, main change, force update or branch deletion.
