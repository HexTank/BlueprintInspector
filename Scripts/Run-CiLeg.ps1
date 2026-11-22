<#
.SYNOPSIS
    Builds the plugin for one engine and runs the automation suite, as the CI engine legs do.

.DESCRIPTION
    Reproduces the manual leg the CI engine jobs run so CI and a
    local matrix run agree:

      1. resolve the engine root, the host project and (inside the suite script) that engine's -Cmd editor;
      2. clear the per-engine intermediates, because UBT otherwise reuses another engine's generated
         headers and the build fails with FTypeConstructFunc/UECodeGen_Private mismatches;
      3. point the host's Plugins\BlueprintInspector link at this checkout for the run, so the leg builds
         the sources under test rather than whatever checkout the link normally targets (restored
         afterwards; a host that has a real directory instead of a link is built in place);
      4. force the host's engine input classes, because an engine without EnhancedInput cannot load the
         EnhancedInput classes and a 5.x editor can write them back on exit (restored afterwards);
      5. Build.bat -Build Plugin -NoPause with UE_ROOT set to the resolved engine;
      6. run the suite and decide pass/fail from its log.

.PARAMETER Engine
    Engine version to build against, for example 4.25 or 5.8.

.PARAMETER EngineRoot
    Engine directory to use instead of resolving -Engine.

.PARAMETER Project
    Host .uproject. Defaults to BPI_HOST_PROJECT.

.PARAMETER LogPath
    Where the suite log goes. Defaults to a file in the temporary directory.

.PARAMETER KeepIntermediate
    Skip the intermediate cleanup. Only safe when the previous leg used the same engine.

.EXAMPLE
    ./Scripts/Run-CiLeg.ps1 -Engine 5.8 -Project C:\Projects\MyProject\MyProject.uproject
#>

#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Engine,
    [string]$EngineRoot,
    [string]$Project,
    [int]$ExpectedTests = 75,
    [string]$LogPath,
    [switch]$KeepIntermediate,
    [switch]$GoldenUpdate,
    [switch]$GoldenVariant,
    [switch]$BuildGameTarget,
    [string]$GameConfiguration = 'Development'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'EnginePaths.ps1')

function Get-ReparsePointTarget {
    param([Parameter(Mandatory)][string]$Path)

    $output = & fsutil.exe reparsepoint query $Path 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    foreach ($line in $output) {
        if ($line -match 'Substitute Name:\s*(.+)$') {
            return ($Matches[1].Trim() -replace '^\\\?\?\\', '')
        }
    }
    return $null
}

function Set-ReparsePointTarget {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Target
    )

    # Directory.Delete($Path, $false) drops the junction/symlink without touching its target.
    [IO.Directory]::Delete($Path, $false)
    New-Item -ItemType Junction -Path $Path -Target $Target -ErrorAction Stop | Out-Null
}

function Get-GameTargetName {
    param([Parameter(Mandatory)][string]$ProjectDirectory)

    $sourceDirectory = Join-Path $ProjectDirectory 'Source'
    if (-not (Test-Path -LiteralPath $sourceDirectory)) { return $null }

    foreach ($file in Get-ChildItem -LiteralPath $sourceDirectory -Filter '*.Target.cs' -File -ErrorAction SilentlyContinue) {
        if ((Get-Content -LiteralPath $file.FullName -Raw) -match 'TargetType\.Game') {
            return [IO.Path]::GetFileNameWithoutExtension([IO.Path]::GetFileNameWithoutExtension($file.Name))
        }
    }
    return $null
}

$exitCode = 0
$pluginLink = $null
$originalPluginTarget = $null
$pluginLinkRetargeted = $false
$inputIni = $null
$originalInputIni = $null

try {
    $root = Resolve-EngineRoot -EngineVersion $Engine -EngineRoot $EngineRoot
    $version = Get-EngineVersion -EngineRoot $root

    if (-not $Project) { $Project = $env:BPI_HOST_PROJECT }
    if (-not $Project) {
        throw 'Pass -Project=<path to the host .uproject> or set BPI_HOST_PROJECT.'
    }
    $projectPath = [IO.Path]::GetFullPath($Project)
    if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
        throw "Host project not found: $projectPath"
    }
    $projectDir = Split-Path -Parent $projectPath

    if (-not $LogPath) {
        $LogPath = Join-Path ([IO.Path]::GetTempPath()) "BlueprintInspector-suite-$version.log"
    }
    $LogPath = [IO.Path]::GetFullPath($LogPath)

    $env:UE_ROOT = $root

    if (-not $KeepIntermediate) {
        $targets = @(
            (Join-Path $repo 'Intermediate')
            (Join-Path $projectDir 'Intermediate\Build\Win64\UE4Editor')
            (Join-Path $projectDir 'Intermediate\Build\Win64\UnrealEditor')
            (Join-Path $projectDir 'Intermediate\Build\BuildRules')
        )
        foreach ($target in $targets) {
            if (-not (Test-Path -LiteralPath $target)) { continue }
            Write-Host "[*] Clearing $target"
            try {
                Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
            }
            catch {
                throw "Could not clear $target ($($_.Exception.Message)). Close any editor or UnrealBuildTool process and retry."
            }
        }
    }

    $pluginLink = Join-Path $projectDir 'Plugins\BlueprintInspector'
    if (Test-Path -LiteralPath $pluginLink) {
        $pluginItem = Get-Item -LiteralPath $pluginLink -Force
        if ($pluginItem.LinkType) {
            $originalPluginTarget = Get-ReparsePointTarget -Path $pluginLink
            if (-not $originalPluginTarget) {
                throw "Could not read the target of the host plugin link: $pluginLink"
            }
            $originalPluginTarget = [IO.Path]::GetFullPath($originalPluginTarget)
            if ($originalPluginTarget -ne [IO.Path]::GetFullPath($repo)) {
                Write-Host '[*] Pointing the host plugin link at this checkout'
                Write-Host "    $pluginLink -> $repo"
                Set-ReparsePointTarget -Path $pluginLink -Target $repo
                $pluginLinkRetargeted = $true
            }
        }
        else {
            Write-Host "[WARNING] The host plugin path is not a link; the leg will build it in place: $pluginLink" -ForegroundColor Yellow
        }
    }
    else {
        Write-Host "[WARNING] The host has no Plugins\BlueprintInspector link: $pluginLink" -ForegroundColor Yellow
    }

    # An engine without EnhancedInput cannot load the EnhancedInput classes, and a 5.x editor can write
    # them back into the host config on exit. That covers every 4.x engine and 5.0, which predates the
    # plugin (it first ships in UE_5.1/Engine/Plugins); without this, a 5.0 leg boots with an input
    # component it cannot resolve and DapAudit fails. From 5.1 on the host keeps its own input config,
    # because forcing the engine classes there breaks a project that requires its own input component.
    $inputIni = Join-Path $projectDir 'Config\DefaultInput.ini'
    if ([version]$version -lt [version]'5.1' -and (Test-Path -LiteralPath $inputIni)) {
        $originalInputIni = [IO.File]::ReadAllText($inputIni)
        $patchedInputIni = [regex]::Replace($originalInputIni, '(?m)^DefaultPlayerInputClass=.*$',
            'DefaultPlayerInputClass=/Script/Engine.PlayerInput')
        $patchedInputIni = [regex]::Replace($patchedInputIni, '(?m)^DefaultInputComponentClass=.*$',
            'DefaultInputComponentClass=/Script/Engine.InputComponent')
        if ($patchedInputIni -ne $originalInputIni) {
            Write-Host '[*] Using the engine input classes in the host config'
            [IO.File]::WriteAllText($inputIni, $patchedInputIni)
        }
    }

    Write-Host "=== Build (UE $version) ==="
    & (Join-Path $repo 'Build.bat') -Build Plugin -NoPause "-Project=$projectPath"
    if ($LASTEXITCODE -ne 0) {
        throw "Build.bat -Build Plugin failed for UE $version (exit $LASTEXITCODE)."
    }

    if ($BuildGameTarget) {
        $gameTarget = Get-GameTargetName -ProjectDirectory $projectDir
        if (-not $gameTarget) {
            throw "No game target found under $projectDir\Source. A plugin that compiles for the editor can still fail in a game target."
        }

        Write-Host "=== Build game target $gameTarget (UE $version, $GameConfiguration) ==="
        & (Join-Path $repo 'Build.bat') -Build Plugin -NoPause "-Project=$projectPath" "-Target=$gameTarget" "-Configuration=$GameConfiguration"
        if ($LASTEXITCODE -ne 0) {
            throw "Build.bat -Target=$gameTarget failed for UE $version (exit $LASTEXITCODE)."
        }
    }

    Write-Host "=== Suite (UE $version) ==="
    & (Join-Path $PSScriptRoot 'Run-AutomationSuite.ps1') -Engine $version -EngineRoot $root -Project $projectPath -ExpectedTests $ExpectedTests -LogPath $LogPath -GoldenUpdate:$GoldenUpdate -GoldenVariant:$GoldenVariant
    $suiteExit = $LASTEXITCODE

    Write-Host "[*] Suite log: $LogPath"
    if ($suiteExit -ne 0) {
        throw "The automation suite failed on UE $version (see $LogPath)."
    }
}
catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
finally {
    if ($pluginLinkRetargeted -and $originalPluginTarget) {
        Write-Host '[*] Restoring the host plugin link'
        Write-Host "    $pluginLink -> $originalPluginTarget"
        try {
            Set-ReparsePointTarget -Path $pluginLink -Target $originalPluginTarget
        }
        catch {
            Write-Host "[ERROR] Could not restore the host plugin link: $($_.Exception.Message)" -ForegroundColor Red
            $exitCode = 1
        }
    }

    if ($inputIni -and $null -ne $originalInputIni) {
        try {
            [IO.File]::WriteAllText($inputIni, $originalInputIni)
        }
        catch {
            Write-Host "[ERROR] Could not restore the host input config: $($_.Exception.Message)" -ForegroundColor Red
            $exitCode = 1
        }
    }
}

exit $exitCode
