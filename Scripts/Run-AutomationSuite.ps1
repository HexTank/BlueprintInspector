<#
.SYNOPSIS
    Runs the BlueprintInspector automation suite against a host project and fails unless every test passes.

.DESCRIPTION
    Runs the suite command the CI engine jobs use:

        <engine>\Engine\Binaries\Win64\<editor>-Cmd.exe <host>.uproject ^
          -ExecCmds="Automation RunTests BlueprintInspector;Quit" -unattended -nopause -nullrhi -nosplash

    The verdict comes from the log, not from the editor's exit code: 4.x can exit non-zero while
    shutting the editor down after a clean run, and an editor that dies mid-suite can still exit 0. The
    script therefore requires exactly -ExpectedTests "Test Completed. Result={Passed}" (UE 4.x) or
    "Result={Success}" (UE 5.x) lines and no non-passing result, and treats a missing
    "**** TEST COMPLETE ****" banner as a warning.

    The editor writes the log to -LogPath via -abslog, which also keeps the host project's own
    Saved\Logs tidy.

.PARAMETER Engine
    Engine version to run, for example 5.8. Resolved through Scripts\EnginePaths.ps1.

.PARAMETER EngineRoot
    Engine directory to use instead of resolving -Engine. Must match -Engine when both are given.

.PARAMETER Project
    Host .uproject. Defaults to BPI_HOST_PROJECT.

.PARAMETER ExpectedTests
    Number of tests the suite must report as passed. The suite records 75.

.PARAMETER LogPath
    Where the editor writes the suite log. Defaults to a file in the temporary directory.

.PARAMETER Replay
    Parse an existing -LogPath and report the verdict without starting an engine.

.EXAMPLE
    ./Scripts/Run-AutomationSuite.ps1 -Engine 5.8 -Project C:\Projects\MyProject\MyProject.uproject

.EXAMPLE
    ./Scripts/Run-AutomationSuite.ps1 -LogPath .\suite.log -Replay
#>

#Requires -Version 5.1

[CmdletBinding()]
param(
    [string]$Engine,
    [string]$EngineRoot,
    [string]$Project,
    [int]$ExpectedTests = 75,
    [string]$Filter = 'BlueprintInspector',
    [int]$TimeoutMinutes = 45,
    [string]$LogPath,
    [switch]$Replay,
    [switch]$GoldenUpdate,
    [switch]$GoldenVariant
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'EnginePaths.ps1')

function Get-AutomationResults {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Suite log not found: $Path"
    }

    $passed = 0
    $failed = 0
    $other = 0
    $nonPassed = New-Object System.Collections.ArrayList
    $complete = $false
    $reportedExitCode = $null

    foreach ($line in [IO.File]::ReadLines($Path)) {
        if ($line -match 'Test Completed\. Result=\{([^}]*)\} Name=\{([^}]*)\}') {
            $verdict = $Matches[1]
            $name = $Matches[2]
            if ($verdict -eq 'Passed' -or $verdict -eq 'Success') {
                $passed++
            }
            elseif ($verdict -eq 'Fail' -or $verdict -eq 'Failed') {
                $failed++
                [void]$nonPassed.Add("$name ($verdict)")
            }
            else {
                $other++
                [void]$nonPassed.Add("$name ($verdict)")
            }
        }
        elseif ($line -match 'TEST COMPLETE\. EXIT CODE:\s*(-?\d+)') {
            $complete = $true
            $reportedExitCode = [int]$Matches[1]
        }
    }

    return [pscustomobject]@{
        Passed           = $passed
        Failed           = $failed
        Other            = $other
        Total            = $passed + $failed + $other
        NonPassed        = @($nonPassed)
        Complete         = $complete
        ReportedExitCode = $reportedExitCode
    }
}

function Get-SuiteIssues {
    param(
        $Results,
        [int]$Expected,
        [string]$Path
    )

    $issues = New-Object System.Collections.ArrayList

    if ($Results.Total -lt $Expected) {
        [void]$issues.Add("the suite reported $($Results.Total) of the expected $Expected tests, so the run aborted, was filtered, or the log is truncated")
    }
    if ($Results.NonPassed.Count -gt 0) {
        [void]$issues.Add("$($Results.NonPassed.Count) test(s) did not pass: $($Results.NonPassed -join '; ')")
    }
    if (-not $Results.Complete) {
        Write-Warning "No '**** TEST COMPLETE ****' line in $Path; the editor may have shut down before automation finished."
    }
    elseif ($Results.ReportedExitCode -ne 0) {
        Write-Warning "The editor reported automation exit code $($Results.ReportedExitCode)."
    }

    return ,$issues
}

$exitCode = 0

try {
    if (-not $LogPath) {
        if ($Replay) { throw '-Replay needs -LogPath pointing at an existing suite log.' }
        $label = if ($Engine) { $Engine } else { 'engine' }
        $LogPath = Join-Path ([IO.Path]::GetTempPath()) "BlueprintInspector-suite-$label.log"
    }
    $LogPath = [IO.Path]::GetFullPath($LogPath)

    if ($Replay) {
        $engineLabel = 'replay'
        Write-Host "=== BlueprintInspector suite ($engineLabel) ==="
        Write-Host "[*] Log:     $LogPath"
        $results = Get-AutomationResults -Path $LogPath
    }
    else {
        $root = Resolve-EngineRoot -EngineVersion $Engine -EngineRoot $EngineRoot
        $version = Get-EngineVersion -EngineRoot $root
        $engineLabel = "UE $version"
        $editor = Resolve-AutomationEditor -EngineRoot $root

        if (-not $Project) { $Project = $env:BPI_HOST_PROJECT }
        if (-not $Project) {
            throw 'Pass -Project=<path to the host .uproject> or set BPI_HOST_PROJECT.'
        }
        $projectPath = [IO.Path]::GetFullPath($Project)
        if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
            throw "Host project not found: $projectPath"
        }

        # Bind an ephemeral loopback port for this run and hand it to the editor. The adapter reads it from
        # the command line, and so do the DAP tests, so a suite cannot collide with an editor that is
        # already listening on the default port.
        $portProbe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
        $portProbe.Start()
        $dapPort = ([System.Net.IPEndPoint]$portProbe.LocalEndpoint).Port
        $portProbe.Stop()

        $logDir = Split-Path -Parent $LogPath
        if ($logDir -and -not (Test-Path -LiteralPath $logDir)) {
            New-Item -ItemType Directory -Path $logDir -Force | Out-Null
        }
        if (Test-Path -LiteralPath $LogPath) { Remove-Item -LiteralPath $LogPath -Force }

        $suiteArgs = @(
            "`"$projectPath`""
            ('-ExecCmds="Automation RunTests {0};Quit"' -f $Filter)
            "-BlueprintInspectorDapPort=$dapPort"
            '-unattended'
            '-nopause'
            '-nullrhi'
            '-nosplash'
            '-nocrashreports'
            '-ini:Engine:[Kismet]:bCanSuppressAccessViolation=true'
            "-abslog=`"$LogPath`""
        )
        if ($GoldenUpdate) {
            $suiteArgs += '-BlueprintGoldenUpdate'
        }
        if ($GoldenVariant) {
            $suiteArgs += '-BlueprintGoldenVariant'
        }

        Write-Host "=== BlueprintInspector suite ($engineLabel) ==="
        Write-Host "[*] Editor:   $editor"
        Write-Host "[*] Project:  $projectPath"
        Write-Host "[*] DAP port: $dapPort"
        Write-Host "[*] Log:      $LogPath"

        # Start-Process -PassThru leaves ExitCode empty on Windows PowerShell 5.1, so start the editor
        # through ProcessStartInfo instead; $suiteArgs already carries the quoting it needs.
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $editor
        $startInfo.Arguments = ($suiteArgs -join ' ')
        $startInfo.UseShellExecute = $false
        $startInfo.WorkingDirectory = (Split-Path -Parent $projectPath)

        $process = [System.Diagnostics.Process]::Start($startInfo)
        $timeoutMs = [int]([Math]::Max(1, $TimeoutMinutes) * 60 * 1000)

        if (-not $process.WaitForExit($timeoutMs)) {
            Write-Warning "The suite did not finish within $TimeoutMinutes minute(s); killing pid $($process.Id) and its children."
            & taskkill.exe /PID $process.Id /T /F 2>&1 | Out-Null
            [void]$process.WaitForExit(30000)
            throw "The suite timed out after $TimeoutMinutes minute(s). Partial log: $LogPath"
        }

        Write-Host "[*] Editor exit code: $($process.ExitCode)"
        if ($process.ExitCode -ne 0) {
            Write-Warning "The editor exited with $($process.ExitCode); the verdict below comes from the log."
        }

        $results = Get-AutomationResults -Path $LogPath
    }

    Write-Host "[*] ${engineLabel}: $($results.Passed)/$($results.Total) passed, $($results.Failed) failed, $($results.Other) other (expected $ExpectedTests)"

    $issues = Get-SuiteIssues -Results $results -Expected $ExpectedTests -Path $LogPath
    if ($issues.Count -gt 0) {
        foreach ($issue in $issues) { Write-Host "[ERROR] $issue" -ForegroundColor Red }
        $exitCode = 1
    }
    else {
        Write-Host "[OK] Suite passed on $engineLabel." -ForegroundColor Green
    }
}
catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}

exit $exitCode
