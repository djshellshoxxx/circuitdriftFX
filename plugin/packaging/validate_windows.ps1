# Shared CI/release gate. Download and hash verification of pluginval precede this script.
# Invoke with pwsh -File, or from a workflow's pwsh step. A failed gate throws.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $ValidatorPath,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $BundleDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $LogDirectory,

    [ValidateRange(1000, 300000)]
    [int] $TimeoutMilliseconds = 300000
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$validator = Get-Item -LiteralPath $ValidatorPath
if ($validator.PSIsContainer) { throw "Validator is a directory: $ValidatorPath" }
$bundleRoot = Get-Item -LiteralPath $BundleDirectory
if (-not $bundleRoot.PSIsContainer) { throw "Bundle directory is not a directory: $BundleDirectory" }

# Staging produces precisely these two products. A missing product must not turn
# into a successful validation of only the other product (or an empty loop).
$expectedNames = @('CircuitDrift.vst3', 'CircuitDrift LITE.vst3')
$bundles = @(Get-ChildItem -LiteralPath $bundleRoot.FullName -Directory -Force)
$unexpected = @($bundles | Where-Object { $_.Name -notin $expectedNames })
if ($bundles.Count -ne $expectedNames.Count -or $unexpected.Count -ne 0) {
    $foundNames = @($bundles | ForEach-Object Name)
    throw "Expected exactly CircuitDrift.vst3 and CircuitDrift LITE.vst3; found: $($foundNames -join ', ')"
}
$bundles = @($expectedNames | ForEach-Object { Get-Item -LiteralPath (Join-Path $bundleRoot.FullName $_) })

$logs = New-Item -ItemType Directory -Path $LogDirectory -Force
# A new run directory and one empty directory per bundle make stale logs from
# another attempt or product ineligible, while retaining earlier diagnostics.
$runName = 'run-{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'), [Guid]::NewGuid().ToString('N')
$runLogs = New-Item -ItemType Directory -Path (Join-Path $logs.FullName $runName)
$results = [System.Collections.Generic.List[object]]::new()
$failed = $false
foreach ($bundle in $bundles) {
    $bundleLogs = New-Item -ItemType Directory -Path (Join-Path $runLogs.FullName $bundle.Name)
    # pluginval is a GUI-subsystem executable on Windows. Wait for the completed
    # process object, never the stale LASTEXITCODE of a detached '&' launch.
    # Start-Process joins ArgumentList into one command line; retain quotes for
    # both Windows paths, including the space in the LITE product name.
    $arguments = '--strictness-level 5 --validate-in-process --timeout-ms {0} --output-dir "{1}" "{2}"' -f $TimeoutMilliseconds, $bundleLogs.FullName, $bundle.FullName
    $exitCode = $null
    $launchError = $null
    try {
        $process = Start-Process -FilePath $validator.FullName -ArgumentList $arguments -Wait -PassThru -NoNewWindow
        $exitCode = $process.ExitCode
    }
    catch {
        $launchError = $_.Exception.Message
    }

    $reports = @(Get-ChildItem -LiteralPath $bundleLogs.FullName -Filter '*.txt' -File)
    $emptyReports = @($reports | Where-Object { $_.Length -eq 0 })
    $passed = $null -eq $launchError -and $null -ne $exitCode -and $exitCode -eq 0 -and
        $reports.Count -gt 0 -and $emptyReports.Count -eq 0
    $results.Add([pscustomobject]@{
        bundle = $bundle.Name
        passed = $passed
        exitCode = $exitCode
        launchError = $launchError
        reports = @($reports | ForEach-Object { [pscustomobject]@{ name = $_.Name; bytes = $_.Length } })
    })
    if (-not $passed) {
        $failed = $true
        Write-Host "pluginval failed for $($bundle.Name): exit=$exitCode, reports=$($reports.Count), empty=$($emptyReports.Count), launchError=$launchError"
        foreach ($report in $reports) { Get-Content -LiteralPath $report.FullName -Tail 80 }
    }
    else {
        Write-Host "pluginval passed for $($bundle.Name): exit=$exitCode, reports=$($reports.Count)"
    }
}

[pscustomobject]@{
    validator = $validator.FullName
    strictness = 5
    timeoutMilliseconds = $TimeoutMilliseconds
    passed = -not $failed
    bundles = $results.ToArray()
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runLogs.FullName 'gate-result.json') -Encoding utf8

if ($failed) { throw "pluginval validation failed; reports retained in $($runLogs.FullName)" }
Write-Host "Both VST3 products completed pluginval; reports retained in $($runLogs.FullName)"
