# In-memory session checkpoint writer preflight — 2026-11-20 UTC

## Scoped engineering finding

The version-1 `loadState` parser rejects checkpoint vectors above 4096 electrical, 4096 thermal or 64 RNG entries, nonfinite dynamic values, invalid Fs/oversampling and invalid checkpoint identity fields. The previously public `saveState` path accepted directly constructed `EngineCheckpoint` objects without equivalent preflight, potentially emitting unreadable or very large session payloads. `decideRestore` was hardened separately in draft PRs #21/#22, but that does not validate serialization.

## Child branch change

- Preflight checkpoint fields before constructing a JSON output tree: nonempty 64-byte-limited identity strings, finite 8–768 kHz sample rate, supported power-of-two oversampling, safe sample index, 4096/4096/64 vector bounds and finite electrical/thermal values.
- Reject attaching a checkpoint to a scene via `saveState`. Scene publication still intentionally strips checkpoints (the `forPublish` path is unchanged).
- Preserve the distinction between *serializable* and *restorable*: a checkpoint with a different engine revision can still be saved; only `decideRestore` determines whether it can be resumed by the actual prepared runtime.
- Extend `state_safety` with malformed in-memory persistence regressions and a valid session roundtrip.

## Exact-source qualification and boundaries

Parent draft PR #22, source `7a95379554aba5d3d42f441436e58cae43ddec4b`, completed [run #44742353994](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44742353994) with Linux and Windows jobs successful, including strictness-5 VST3 pluginval. This result qualifies the **parent only**. The child requires its own Linux sanitized CTest and Windows/pluginval gates.

Review ownership: code review should compare preflight with the parser's schema-v1 limits; bug hunt should test boundary lengths and the publish-scene bypass; defensive review should inspect malformed direct programmatic checkpoints and failure atomicity. Independent reviewer workers were not exposed through the available connector. No reviewer signoff is claimed.

The change does not connect audio-owned runtime/control/seed capture to the host, does not serialize exact live plugin session continuation and does not close G12. No merge, release, deployment, source-branch deletion or modification of PR #16/main is authorized or performed.
