# Deterministic wrapper regressions: no third-party executable is launched.
# The real Windows/pluginval run remains the host-compatibility acceptance gate.
[CmdletBinding()]
param()

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$gateScript = Join-Path $PSScriptRoot 'validate_windows.ps1'
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('cdfx pluginval gate ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null

# This script-local function is visible to the child gate script. It stands in
# for the OS process boundary and returns controlled completed-process results.
function Start-Process {
    [CmdletBinding()]
    param(
        [string] $FilePath,
        [string[]] $ArgumentList,
        [switch] $Wait,
        [switch] $PassThru,
        [switch] $NoNewWindow
    )
    if (-not $Wait -or -not $PassThru -or -not $NoNewWindow) {
        throw 'Gate must wait for the process and request its completed result.'
    }
    if ($FilePath -ne $fixtureState.Validator) { throw 'Unexpected validator path.' }
    $commandLine = $ArgumentList -join ' '
    if ($commandLine -notmatch '^--strictness-level 5 --validate-in-process --timeout-ms ([0-9]+) --output-dir "([^"]+)" "([^"]+)"$') {
        throw "Incorrect validator arguments or unquoted paths: $commandLine"
    }
    if ([int] $Matches[1] -ne $fixtureState.Timeout) { throw 'Incorrect validator timeout.' }
    $outputDirectory = $Matches[2]
    $bundle = Get-Item -LiteralPath $Matches[3]
    if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) { throw 'Missing report directory.' }
    if (@(Get-ChildItem -LiteralPath $outputDirectory -Force).Count -ne 0) { throw 'Report directory is not fresh.' }
    $fixtureState.Calls.Add([pscustomobject]@{ bundle = $bundle.Name; outputDirectory = $outputDirectory })
    $isTarget = $bundle.Name -eq $fixtureState.Target
    if ($isTarget -and $fixtureState.Mode -eq 'launch-error') { throw 'Synthetic process launch failure.' }
    if ($fixtureState.Mode -ne 'no-reports-both' -and -not ($isTarget -and $fixtureState.Mode -eq 'no-reports')) {
        $report = Join-Path $outputDirectory 'validation.txt'
        if ($isTarget -and $fixtureState.Mode -eq 'empty-report') {
            New-Item -ItemType File -Path $report | Out-Null
        }
        else {
            Set-Content -LiteralPath $report -Value 'Synthetic completed validator report.'
        }
        if ($isTarget -and $fixtureState.Mode -eq 'mixed-empty-report') {
            New-Item -ItemType File -Path (Join-Path $outputDirectory 'empty.txt') | Out-Null
        }
    }
    if ($isTarget -and $fixtureState.Mode -eq 'nonzero-exit') { return [pscustomobject]@{ ExitCode = 7 } }
    if ($isTarget -and $fixtureState.Mode -eq 'missing-exit') { return [pscustomobject]@{ ExitCode = $null } }
    return [pscustomobject]@{ ExitCode = 0 }
}

function Invoke-GateCase {
    param(
        [string] $Name,
        [bool] $ExpectPass,
        [string] $Mode = 'success',
        [string] $Target = 'CircuitDrift.vst3',
        [string[]] $BundleNames = @('CircuitDrift.vst3', 'CircuitDrift LITE.vst3'),
        [string[]] $FileNames = @(),
        [int] $ExpectedCalls = 2,
        [int] $TimeoutMilliseconds = 300000
    )
    $caseRoot = New-Item -ItemType Directory -Path (Join-Path $testRoot $Name)
    $bundleRoot = New-Item -ItemType Directory -Path (Join-Path $caseRoot.FullName 'staged plugins')
    foreach ($bundleName in $BundleNames) { New-Item -ItemType Directory -Path (Join-Path $bundleRoot.FullName $bundleName) | Out-Null }
    foreach ($fileName in $FileNames) { New-Item -ItemType File -Path (Join-Path $bundleRoot.FullName $fileName) | Out-Null }
    $validator = New-Item -ItemType File -Path (Join-Path $caseRoot.FullName 'synthetic validator.exe')
    $logs = New-Item -ItemType Directory -Path (Join-Path $caseRoot.FullName 'validator logs')
    # Prior nonempty reports must never satisfy this invocation or be deleted.
    $oldLogs = New-Item -ItemType Directory -Path (Join-Path $logs.FullName 'previous-run')
    $oldReport = Join-Path $oldLogs.FullName 'old-validation.txt'
    Set-Content -LiteralPath $oldReport -Value 'Stale successful report.'
    $fixtureState = @{
        Mode = $Mode
        Target = $Target
        Validator = $validator.FullName
        Timeout = $TimeoutMilliseconds
        Calls = [System.Collections.Generic.List[object]]::new()
    }
    $actualPass = $true
    $failureMessage = ''
    try {
        & $gateScript -ValidatorPath $validator.FullName -BundleDirectory $bundleRoot.FullName -LogDirectory $logs.FullName -TimeoutMilliseconds $TimeoutMilliseconds
    }
    catch {
        $actualPass = $false
        $failureMessage = $_.Exception.Message
    }
    if ($actualPass -ne $ExpectPass) { throw "Case '$Name': expected pass=$ExpectPass, got pass=$actualPass ($failureMessage)" }
    if ($fixtureState.Calls.Count -ne $ExpectedCalls) { throw "Case '$Name': expected $ExpectedCalls process calls, got $($fixtureState.Calls.Count) ($failureMessage)" }
    if ((Get-Content -LiteralPath $oldReport -Raw).Trim() -ne 'Stale successful report.') { throw "Case '$Name': prior report changed." }
    if ($ExpectedCalls -eq 2) {
        if ($fixtureState.Calls[0].bundle -ne 'CircuitDrift.vst3' -or $fixtureState.Calls[1].bundle -ne 'CircuitDrift LITE.vst3') {
            throw "Case '$Name': both expected products were not validated."
        }
        if ($fixtureState.Calls[0].outputDirectory -eq $fixtureState.Calls[1].outputDirectory) { throw "Case '$Name': products shared reports." }
        $summaries = @(Get-ChildItem -LiteralPath $logs.FullName -Filter 'gate-result.json' -File -Recurse)
        if ($summaries.Count -ne 1) { throw "Case '$Name': missing gate summary." }
        $summary = Get-Content -LiteralPath $summaries[0].FullName -Raw | ConvertFrom-Json
        if ($summary.passed -ne $ExpectPass -or $summary.bundles.Count -ne 2 -or $summary.timeoutMilliseconds -ne $TimeoutMilliseconds) { throw "Case '$Name': incorrect persisted result." }
    }
    Write-Host "PASS: $Name"
}

try {
    Invoke-GateCase -Name 'both-products-and-spaced-paths' -ExpectPass $true
    Invoke-GateCase -Name 'release-timeout' -ExpectPass $true -TimeoutMilliseconds 120000
    Invoke-GateCase -Name 'full-exit-failure' -ExpectPass $false -Mode 'nonzero-exit'
    Invoke-GateCase -Name 'lite-exit-failure' -ExpectPass $false -Mode 'nonzero-exit' -Target 'CircuitDrift LITE.vst3'
    Invoke-GateCase -Name 'missing-exit' -ExpectPass $false -Mode 'missing-exit'
    Invoke-GateCase -Name 'missing-report' -ExpectPass $false -Mode 'no-reports'
    Invoke-GateCase -Name 'empty-report' -ExpectPass $false -Mode 'empty-report'
    Invoke-GateCase -Name 'mixed-empty-report' -ExpectPass $false -Mode 'mixed-empty-report'
    Invoke-GateCase -Name 'stale-reports-only' -ExpectPass $false -Mode 'no-reports-both'
    Invoke-GateCase -Name 'launch-failure' -ExpectPass $false -Mode 'launch-error'
    Invoke-GateCase -Name 'missing-full' -ExpectPass $false -BundleNames @('CircuitDrift LITE.vst3') -ExpectedCalls 0
    Invoke-GateCase -Name 'missing-lite' -ExpectPass $false -BundleNames @('CircuitDrift.vst3') -ExpectedCalls 0
    Invoke-GateCase -Name 'empty-staging' -ExpectPass $false -BundleNames @() -ExpectedCalls 0
    Invoke-GateCase -Name 'unexpected-replacement' -ExpectPass $false -BundleNames @('CircuitDrift.vst3', 'Unrelated.vst3') -ExpectedCalls 0
    Invoke-GateCase -Name 'unexpected-extra' -ExpectPass $false -BundleNames @('CircuitDrift.vst3', 'CircuitDrift LITE.vst3', 'Unrelated.vst3') -ExpectedCalls 0
    Invoke-GateCase -Name 'file-instead-of-bundle' -ExpectPass $false -BundleNames @('CircuitDrift LITE.vst3') -FileNames @('CircuitDrift.vst3') -ExpectedCalls 0
    Write-Host 'Windows pluginval wrapper: all 16 synthetic cases passed.'
}
finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
