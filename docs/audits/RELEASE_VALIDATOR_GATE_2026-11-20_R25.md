# Shared Windows validation gate — R25, 2026-11-20

## Finding and repair

R24 reviewed source `b84d2ba119c03f079cd74886cbd066a2da6013f6` and found that
the release workflow still used the direct GUI executable invocation already
shown defective in the [earlier CI audit](WINDOWS_PLUGINVAL_2026-11-20.md).
Its `$LASTEXITCODE` check did not establish that pluginval had completed.

Both workflows now call
[`validate_windows.ps1`](../../plugin/packaging/validate_windows.ps1). It:

- Requires exactly the `CircuitDrift.vst3` and `CircuitDrift LITE.vst3` directories
  produced by staging, rejecting missing or unexpected directories before launch.
- Runs each product with strictness 5, in-process validation and the existing
  workflow-specific pluginval timeouts: CI retains 300,000 ms and release retains
  120,000 ms. Both workflow steps also have a 15-minute limit.
- Uses `Start-Process -Wait -PassThru -NoNewWindow` and reads the completed process
  exit code. Both paths remain quoted in the single argument string, including
  the space in the LITE product name.
- Creates a new run directory and a separate empty report directory per product.
  A previous run's report or the other product's report cannot satisfy the gate.
- Requires exit code zero and at least one nonempty `.txt` report, rejecting an
  empty report even when another report is nonempty. Launch errors and absent
  exit status fail the gate as well.
- Attempts both products after a validator failure, retains reports and records
  `gate-result.json`. Existing reports are not deleted. The workflows' always-run
  artifact steps preserve the complete log directory on failure.

The download URL, expected SHA-256, pinned Actions references, staging checks and
publish permissions are unchanged. The release workflow does not restore a
dependency cache. CI caches only `juce-src` and `clap_juce_extensions-src`, with a
new key namespace and hashes of both dependency CMake files. The companion R25
dependency repair keeps FetchContent subbuild state in the current build tree.

## Verification and remaining acceptance

The [PowerShell regression harness](../../plugin/packaging/test_validate_windows.ps1)
is invoked early in both Windows workflows. It substitutes only the process
boundary with a repository-owned fixture and checks the production script's
verdict, product invocations, path quoting, report isolation and persisted result.
Its 16 cases cover success at both workflow timeouts, either product's nonzero exit, absent exit status,
missing/empty/mixed reports, stale reports, launch failure, empty or incomplete
staging, an unexpected replacement/extra product and a file in place of a bundle.
It does not launch pluginval or establish actual GUI process synchronization.

Executed locally: PyYAML 6.0.3 parsed both workflows; source-contract checks
confirmed both shared call sites, timeouts, early harness execution, retained
always-run report artifacts, immutable Actions references, source-only CI cache
paths and the absence of release cache restore. `git diff --check` passed.

**PowerShell was unavailable locally. The harness has not been executed at this
checkpoint.** A fresh Windows run must execute it, then validate both staged
products with the real pinned pluginval binary. The real release validation path
remains a required gate before any release. No build, validator, release or
deployment was performed by this worker.

## API references

Microsoft's [Start-Process reference](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/start-process)
documents process waiting, the returned process object and preservation of quote
characters in a single `ArgumentList` string. Its
[scope reference](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_scopes)
documents how the test's script-local process fixture is visible to a child
script without changing the production script's interface.
