<#
.SYNOPSIS
    Runs the packaged smoke: package a host project, start it, attach a headless DAP client and exercise the adapter.

.DESCRIPTION
    Release gate 3, made repeatable. Builds a Shipping package of the host project with the plugin in it
    (unless -Executable is given), starts the packaged game with the level and experience, waits for the
    DAP listener, and drives Scripts\dap-smoke-client.js against it.

    The smoke mode sets breakpoints in Blueprints that run during gameplay, waits for a stop, reads the
    stack, scopes and variables, changes one scalar, steps, and detaches. The auth mode runs the negative
    cases: an empty token, a wrong token, a token from a closed connection, a second client while one is
    attached, and a fresh client afterwards.

    The token is published per connection, so the client connects first and reads the file after that;
    reading it before a connection exists fails by design.

.PARAMETER Project
    Host .uproject to package and launch. Defaults to BPI_HOST_PROJECT.

.PARAMETER Engine
    Engine root used for packaging, for example D:\UnrealEngine\UE_5.8. Required with -Package.

.PARAMETER Executable
    Packaged game executable to launch. Skips packaging; defaults to <ArchiveDirectory>\Windows\LyraGame.exe
    after a build.

.PARAMETER ArchiveDirectory
    Where -Package archives the build. Defaults to <ProjectDir>\Packaged-Smoke.

.PARAMETER Map
    Level to open in the packaged game. It must contain Blueprints that run without player input, or no
    breakpoint can be hit after the client attaches.

.PARAMETER Experience
    Lyra-style experience appended to the level URL. Pass an empty string for a plain project.

.PARAMETER Mode
    smoke (default) or auth.

.PARAMETER NullRhi
    Launch with -nullrhi, so no rendering pipeline is created. The auth negatives only need the listener
    and the token file, so this is the way to run them on a host whose GPU driver cannot render the
    Shipping shader set (an NVIDIA driver shader-JIT stack-cookie crash, for example). Do not use it for
    smoke mode: breakpoints in gameplay Blueprints need the game to render and tick.

.PARAMETER BreakTargets
    Comma-separated Blueprint name fragments to break in, for smoke mode.

.PARAMETER SettleSeconds
    How long to wait after the listener opens before the client connects. The plugin starts its listener
    during engine initialisation, so an immediate connect can land while the game is still loading.

.PARAMETER ClientConfig
    Configuration to package. Shipping is the release gate. Development writes a log file, so use it when
    a failure needs the adapter's own messages; a Shipping build logs only if the host target sets
    TargetRules.bUseLoggingInShipping, which is off by default.

.PARAMETER KeepRunning
    Leave the game running instead of stopping it afterwards.

.EXAMPLE
    ./Scripts/Invoke-PackagedSmoke.ps1 -Package -Project D:\LyraStarterGame\LyraStarterGame.uproject

.EXAMPLE
    ./Scripts/Invoke-PackagedSmoke.ps1 -Executable D:\Builds\Windows\LyraGame.exe -Mode auth -KeepRunning
#>

#Requires -Version 5.1

[CmdletBinding()]
param(
    [string]$Project,
    [string]$Engine,
    [string]$Executable,
    [string]$ArchiveDirectory,
    [string]$Map = '/Game/System/FrontEnd/Maps/L_LyraFrontEnd',
    [string]$Experience = 'B_ShooterGame_Elimination',
    [ValidateSet('smoke', 'auth')][string]$Mode = 'smoke',
    [string]$BreakTargets = 'BTS_Shoot,GA_Weapon_Fire,B_Hero_ShooterMannequin',
    [int]$Port = 19021,
    [int]$TimeoutSeconds = 180,
    [int]$SettleSeconds = 15,
    [ValidateSet('Shipping', 'Development')][string]$ClientConfig = 'Shipping',
    [int]$ResX = 1280,
    [int]$ResY = 720,
    [switch]$Package,
    [switch]$NullRhi,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'EnginePaths.ps1')

function Test-Listener {
    param([int]$PortNumber)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $PortNumber, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne(400)) { return $false }
        $client.EndConnect($async)
        return $true
    }
    catch {
        return $false
    }
    finally {
        $client.Close()
    }
}

if (-not $Project) { $Project = $env:BPI_HOST_PROJECT }
if (-not $Project) { throw 'Pass -Project=<host .uproject> or set BPI_HOST_PROJECT.' }
$Project = [IO.Path]::GetFullPath($Project)
if (-not (Test-Path -LiteralPath $Project -PathType Leaf)) { throw "Host project not found: $Project" }
$projectDirectory = Split-Path -Parent $Project

if (-not $ArchiveDirectory) { $ArchiveDirectory = Join-Path $projectDirectory 'Packaged-Smoke' }

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw 'node is not on PATH; the smoke client is Scripts\dap-smoke-client.js.'
}

if (Test-Listener -PortNumber $Port) {
    throw "Something is already listening on 127.0.0.1:$Port. Stop it, then run this again."
}

if ($Package) {
    if (-not $Engine) { throw 'Pass -Engine=<engine root> with -Package, for example D:\UnrealEngine\UE_5.8.' }
    $engineRoot = Resolve-EngineDirectory -Path $Engine -What 'engine root'
    $uat = Join-Path $engineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
    if (-not (Test-Path -LiteralPath $uat)) { throw "RunUAT not found under $engineRoot" }

    Write-Host "[*] Packaging $(Split-Path -Leaf $Project) (Win64 $ClientConfig) into $ArchiveDirectory"
    & $uat BuildCookRun "-project=$Project" -platform=Win64 "-clientconfig=$ClientConfig" -cook -build -stage -pak -archive "-archivedirectory=$ArchiveDirectory" -unattended -nop4 -utf8output
    if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed with exit code $LASTEXITCODE." }
}

if (-not $Executable) {
    $Executable = Join-Path $ArchiveDirectory 'Windows\LyraGame.exe'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Packaged executable not found: $Executable. Pass -Executable, or -Package to build one."
}

$image = [IO.Path]::GetFileNameWithoutExtension($Executable)
$imageNames = @($image, "$image-Win64-Shipping")
$leftovers = Get-Process -Name $imageNames -ErrorAction SilentlyContinue
if ($leftovers) {
    throw "A packaged game is already running (PID $($leftovers.Id -join ', ')). Stop it before running the smoke."
}

$tokenName = if ($Port -eq 19021) { 'dap-token' } else { "dap-token-$Port" }
$tokenPath = Join-Path (Join-Path $env:LOCALAPPDATA 'BlueprintInspector') $tokenName

$url = $Map
if ($Experience) { $url = "$Map`?Experience=$Experience" }

$gameArguments = @($url, '-windowed', "-ResX=$ResX", "-ResY=$ResY", '-nosplash', "-BlueprintInspectorDapPort=$Port")
if ($NullRhi) {
    $gameArguments += '-nullrhi'
    if ($Mode -eq 'smoke') {
        Write-Host '[!] -NullRhi with smoke mode: gameplay Blueprints do not tick, so no breakpoint can be hit.' -ForegroundColor Yellow
    }
}
Write-Host "[*] Launching $(Split-Path -Leaf $Executable): $url"
$game = Start-Process -FilePath $Executable -ArgumentList $gameArguments -WorkingDirectory (Split-Path -Parent $Executable) -PassThru

$verdict = 1
try {
    Write-Host -NoNewline "[*] Waiting for the DAP listener on 127.0.0.1:$Port "
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Listener -PortNumber $Port) { Write-Host ' up'; break }
        if ($game.HasExited) { throw "The packaged game exited with code $($game.ExitCode) before opening the listener." }
        Write-Host -NoNewline '.'
        Start-Sleep -Milliseconds 750
    }
    if (-not (Test-Listener -PortNumber $Port)) {
        throw "The listener did not open within $TimeoutSeconds s. Check the staged Saved\Logs for plugin load errors."
    }

    if ($SettleSeconds -gt 0) {
        Write-Host "[*] Settling for $SettleSeconds s while the game finishes loading"
        Start-Sleep -Seconds $SettleSeconds
    }

    Write-Host "[*] Mode: $Mode"
    $client = Join-Path $PSScriptRoot 'dap-smoke-client.js'
    $env:BPI_PORT = "$Port"

    for ($attempt = 1; $attempt -le 2; $attempt++) {
        & node $client $tokenPath $Mode $BreakTargets
        $verdict = $LASTEXITCODE
        if ($verdict -eq 0) { break }
        if ($attempt -lt 2) {
            # A retry only makes sense while the game is still serving the listener: a second client
            # cannot connect to a process that has exited, and "ECONNREFUSED" would hide the first and
            # real failure behind a consequence.
            if ($game.HasExited) {
                Write-Host "[!] The packaged game exited with code $($game.ExitCode) during attempt $attempt; not retrying." -ForegroundColor Yellow
                break
            }
            Write-Host "[*] Attempt $attempt did not pass; retrying in 10 s" -ForegroundColor Yellow
            Start-Sleep -Seconds 10
        }
    }

    # A crash is a failure even when every client-side check passed: the auth mode's refusals accept any
    # socket error, so a game that died mid-run would otherwise report green.
    if ($verdict -eq 0 -and $game.HasExited) {
        Write-Host "[!] The packaged game exited with code $($game.ExitCode) during the $Mode smoke" -ForegroundColor Yellow
        $verdict = 1
    }
}
finally {
    if (-not $KeepRunning) {
        $running = Get-Process -Name $imageNames -ErrorAction SilentlyContinue
        if ($running) {
            Write-Host "[*] Stopping $($running.ProcessName -join ', ') (PID $($running.Id -join ', '))"
            $running | Stop-Process -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($verdict -ne 0) {
    Write-Host "[FAILED] the packaged $Mode smoke did not pass" -ForegroundColor Red
    exit 1
}

Write-Host "[OK] the packaged $Mode smoke passed" -ForegroundColor Green
exit 0
