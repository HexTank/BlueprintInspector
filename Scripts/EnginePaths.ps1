<#
.SYNOPSIS
    Engine, editor and port resolution shared by the CI scripts.

.DESCRIPTION
    Dot-source this file; it only defines functions.

    Run-AutomationSuite.ps1 and Run-CiLeg.ps1 both need the same answers: which engine root belongs to a
    version, which -Cmd editor binary that engine ships, and whether the DAP port is free. Keeping them
    here means an engine quirk is fixed once. The search order follows the D:\UnrealEngine\UE_<version>
    convention that Build.ps1 uses, and UE_ROOT / BPI_ENGINE_ROOT override it.
#>

function Resolve-EngineDirectory {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string]$What = 'engine root'
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$What not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-EngineVersion {
    param([Parameter(Mandatory)][string]$EngineRoot)

    # Engine/Build/Build.version is authoritative; an installed build's folder name is the fallback.
    $versionFile = Join-Path $EngineRoot 'Engine\Build\Build.version'
    if (Test-Path -LiteralPath $versionFile -PathType Leaf) {
        try {
            $json = Get-Content -LiteralPath $versionFile -Raw | ConvertFrom-Json
            if ($json.PSObject.Properties.Name -contains 'MajorVersion') {
                return ('{0}.{1}' -f $json.MajorVersion, $json.MinorVersion)
            }
        }
        catch {
            throw "Could not read ${versionFile}: $($_.Exception.Message)"
        }
    }

    if ((Split-Path -Leaf $EngineRoot) -match '^UE_(\d+\.\d+)$') {
        return $Matches[1]
    }

    throw "Cannot tell the engine version of ${EngineRoot}: no readable Engine\Build\Build.version and the folder is not named UE_<major>.<minor>."
}

function Resolve-EngineRoot {
    param(
        [string]$EngineVersion,
        [string]$EngineRoot
    )

    if ($EngineRoot) {
        $resolved = Resolve-EngineDirectory -Path $EngineRoot -What 'engine root'
        if ($EngineVersion) {
            $actual = Get-EngineVersion -EngineRoot $resolved
            if ($actual -ne $EngineVersion) {
                throw "-EngineRoot points at UE $actual, but UE $EngineVersion was requested: $resolved"
            }
        }
        return $resolved
    }

    $base = 'D:\UnrealEngine'
    if ($env:BPI_ENGINE_ROOT) { $base = $env:BPI_ENGINE_ROOT }

    if ($EngineVersion) {
        $candidate = Join-Path $base ("UE_" + $EngineVersion)
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }

        # A runner dedicated to one engine sets UE_ROOT instead of using the shared base directory.
        if ($env:UE_ROOT -and (Test-Path -LiteralPath $env:UE_ROOT -PathType Container)) {
            $actual = Get-EngineVersion -EngineRoot $env:UE_ROOT
            if ($actual -eq $EngineVersion) {
                return (Resolve-Path -LiteralPath $env:UE_ROOT).Path
            }
        }

        throw "No UE $EngineVersion under $base (looked for $(Join-Path $base ('UE_' + $EngineVersion))) and UE_ROOT does not point at it. Pass -EngineRoot or set BPI_ENGINE_ROOT."
    }

    if ($env:UE_ROOT) {
        return (Resolve-EngineDirectory -Path $env:UE_ROOT -What 'UE_ROOT')
    }

    throw 'Pass -Engine <major.minor>, -EngineRoot, or set UE_ROOT.'
}

function Resolve-AutomationEditor {
    param([Parameter(Mandatory)][string]$EngineRoot)

    $version = Get-EngineVersion -EngineRoot $EngineRoot
    $major = [int]$version.Split('.')[0]
    $binDir = Join-Path $EngineRoot 'Engine\Binaries\Win64'

    # The suite runs the DebugGame -Cmd editor, which is how the documented matrix runs it; the
    # non-DebugGame editor is only a fallback. 4.x ships UE4Editor*, 5.x UnrealEditor*.
    $names = if ($major -ge 5) {
        @('UnrealEditor-Win64-DebugGame-Cmd.exe', 'UnrealEditor-Cmd.exe')
    }
    else {
        @('UE4Editor-Win64-DebugGame-Cmd.exe', 'UE4Editor-Cmd.exe')
    }

    foreach ($name in $names) {
        $candidate = Join-Path $binDir $name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    throw "No -Cmd editor under $binDir (looked for: $($names -join ', '))."
}
