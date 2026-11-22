<#
.SYNOPSIS
    Stages the plugin as a self-contained folder and zips it for a release.

.DESCRIPTION
    Mirrors Build.ps1 -Build Package but needs no engine: both prebuilt library pairs (v142 for UE 4.x
    and v143 for UE 5.x) are included, so one archive serves the whole supported engine range.

.PARAMETER Version
    Version used in the archive name and INSTALL.md. Defaults to the .uplugin VersionName.

.PARAMETER OutputDirectory
    Where the zip is written. Defaults to Saved.

.PARAMETER CommitHash
    Short commit hash in the archive name; defaults to git rev-parse --short HEAD.

.EXAMPLE
    ./Scripts/New-PluginPackage.ps1 -OutputDirectory C:\Builds
#>

#Requires -Version 5.1

[CmdletBinding()]
param(
    [string]$Version,
    [string]$OutputDirectory,
    [string]$CommitHash
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $descriptor = Get-Content -LiteralPath (Join-Path $repo 'BlueprintInspector.uplugin') -Raw | ConvertFrom-Json
    $Version = if ($descriptor.VersionName) { [string]$descriptor.VersionName } else { "v$($descriptor.Version)" }
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'Saved' }
if (-not $CommitHash) {
    try { $CommitHash = ((& git -C $repo rev-parse --short HEAD 2>$null) | Out-String).Trim() } catch { }
}

$sourceRoot = Join-Path $repo 'Source'
$stagingRoot = Join-Path ([IO.Path]::GetTempPath()) ('BlueprintInspector-package-' + [guid]::NewGuid().ToString('N'))
$staging = Join-Path $stagingRoot 'BlueprintInspector'
New-Item -ItemType Directory -Path $staging -Force | Out-Null

try {
    Copy-Item -LiteralPath (Join-Path $repo 'BlueprintInspector.uplugin') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $repo 'THIRD-PARTY-NOTICES.md') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $repo 'Content') -Destination $staging -Recurse
    $destData = Join-Path $staging 'Data'
    New-Item -ItemType Directory -Path $destData | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'Data\BlueprintLayoutGuide.md') -Destination $destData

    $destSource = Join-Path $staging 'Source'
    New-Item -ItemType Directory -Path $destSource | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'BlueprintInspector') -Destination $destSource -Recurse

    $destThirdParty = Join-Path $destSource 'ThirdParty'
    $destInclude = Join-Path $destThirdParty 'include'
    New-Item -ItemType Directory -Path $destInclude -Force | Out-Null
    Copy-Item -Path (Join-Path $sourceRoot 'ThirdParty\include\*') -Destination $destInclude -Recurse
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'ThirdParty\LICENSE-cppdap.txt') -Destination $destThirdParty
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'ThirdParty\LICENSE-detours.md') -Destination $destThirdParty

    # Ship both pairs; BlueprintInspector.Build.cs picks one from the target engine's major version.
    foreach ($line in @('v142', 'v143')) {
        $destLib = Join-Path $destThirdParty ('lib\' + $line)
        New-Item -ItemType Directory -Path $destLib -Force | Out-Null
        foreach ($lib in @('cppdap.lib', 'detours.lib')) {
            $sourceLib = Join-Path $sourceRoot ('ThirdParty\lib\' + $line + '\' + $lib)
            if (-not (Test-Path -LiteralPath $sourceLib)) { throw "Missing $sourceLib." }
            Copy-Item -LiteralPath $sourceLib -Destination $destLib
        }
    }

    $destExtensionRoot = Join-Path $staging 'VSCodeExtension'
    New-Item -ItemType Directory -Path $destExtensionRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'VSCodeExtension\Launch') -Destination $destExtensionRoot -Recurse
    $destExtension = Join-Path $destExtensionRoot 'Extension'
    New-Item -ItemType Directory -Path $destExtension | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $repo 'VSCodeExtension\Extension') -Force |
        Where-Object { $_.Name -notin @('node_modules', 'out') -and $_.Extension -ne '.vsix' } |
        Copy-Item -Destination $destExtension -Recurse -Force

    $install = @"
# BlueprintInspector $Version

DAP debugger for Unreal Engine Blueprints (Win64). Copy this `BlueprintInspector` folder into
`<YourProject>\Plugins\BlueprintInspector`, keep the folder name, and open the project. The plugin
builds from source on first launch; UE 4.25 through 5.8 are supported.

The module links a prebuilt cppdap and Detours, so no CMake step is needed. Both the v142 (UE 4.x)
and v143 (UE 5.x) library pairs are included. `Content/Tests/OpcodeShowcase` and
`Data/BlueprintLayoutGuide.md` are the automation fixture set; they only matter if you run the
`BlueprintInspector.*` automation tests. `THIRD-PARTY-NOTICES.md` lists the bundled libraries and
their licenses.

See the repository README for the debug configuration and the VS Code extension.
"@
    Set-Content -LiteralPath (Join-Path $staging 'INSTALL.md') -Value $install -Encoding ASCII

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $name = if ($CommitHash) { "BlueprintInspector-$Version-$CommitHash.zip" } else { "BlueprintInspector-$Version.zip" }
    $zip = Join-Path $OutputDirectory $name
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -LiteralPath $staging -DestinationPath $zip -CompressionLevel Optimal
    Write-Output $zip
}
finally {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
}
