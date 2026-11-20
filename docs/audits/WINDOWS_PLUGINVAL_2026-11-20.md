# Windows pluginval CI failure — 2026-11-20

Source: [PR #16 CI run 44662769491](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44662769491), original feature head `e721e9eea044b352546abd3a1b945fa1f3745038`.

- Linux passed. Windows MSVC configure/build, CTest, packaging and artifact upload passed.
- Only the `pluginval (VST3, strictness 5)` step failed (job 136049529086).
- Log timestamps show the wrapper reporting "pluginval failed" for both variants at 03:41:26.46 UTC, before "Started validating" at 03:41:26.71 and 03:41:26.79. Validator output continued after the wrapper verdict; Editor Automation began after the step failed. This establishes that the original wrapper did not wait for validation completion.
- Uploaded artifact 14599312794 contains two zero-byte reports. Neither a plugin pass nor a plugin incompatibility verdict can be concluded from those incomplete logs.
- Actions default pull-request checkout used synthetic merge SHA `33fbe24c62c134998bf6fe080b592772c390300c`, not the named feature head.
- Remediation: wait for actual Windows GUI process completion using `Start-Process -Wait -PassThru`, reject nonzero exit, missing/empty per-bundle reports and absent bundles; checkout and label the exact PR head SHA. Keep pluginval strictness at 5; do not bypass validation.
- Acceptance remains: a fresh exact-head Windows run must produce nonempty pluginval reports and successful tests for both VST3 products. CI execution of this new wrapper has **not yet occurred** at this checkpoint. PowerShell was unavailable for local execution. Other PR child edits remain unqualified until the new run.
