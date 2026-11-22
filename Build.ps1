<#
.SYNOPSIS
    Build driver for the BlueprintInspector plugin.

.DESCRIPTION
    This is the real implementation behind Compile-Plugin.bat, Select-BuildToolchain.bat and
    Build.bat.

    The plugin's MSVC toolset must match the one UnrealBuildTool uses for the module.  UBT 4.25 has
    no -CompilerVersion switch, so the exact MSVC toolset is pinned by temporarily adding
    <WindowsPlatform><CompilerVersion> to the per-user BuildConfiguration.xml and restoring the file
    after the build.  (The -ini:Engine override only feeds [ConfigFile] attributes; the
    toolset-version field is [XmlConfigFile] only.)

    Toolchain choice is stored in Saved\BuildToolchain.cfg.  The module links the prebuilt libraries
    under Source\ThirdParty\lib\<line>\; those are maintained in the repository and there is no
    build step for them.

    Run Build.ps1 -Help for the command line.
#>

param(
    [string]$Build = ''
)

$ErrorActionPreference = 'Stop'

$script:RepoRoot = $PSScriptRoot
if (-not $script:RepoRoot) { $script:RepoRoot = (Get-Location).Path }

$script:ThirdPartyDir = Join-Path $script:RepoRoot 'Source\ThirdParty'
$script:PluginBinDir  = Join-Path $script:RepoRoot 'Binaries\Win64'
$script:SavedDir      = Join-Path $script:RepoRoot 'Saved'
$script:ToolchainCfg  = Join-Path $script:SavedDir 'BuildToolchain.cfg'
$script:EngineStamp   = Join-Path $script:SavedDir 'BuildEngine.stamp'
$script:PluginDir     = $script:RepoRoot
$script:FixturesDir   = Join-Path $script:RepoRoot 'Fixtures'
$script:ActiveFixturesDir = Join-Path $script:RepoRoot 'Content\Tests\OpcodeShowcase'

$toolchainSpec   = 'saved'
$selectToolchain = $false
$listToolchains  = $false
$noPause         = $false
$dryRun          = $false
$showHelp        = $false
$configuration   = 'DebugGame'
$platform        = 'Win64'
$gotConfiguration = $false
$gotPlatform      = $false
$singleFile       = $null
$hostProject      = $null
$projectTarget    = $null
$remaining        = New-Object System.Collections.ArrayList

for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = [string]$args[$i]

    if ($arg -match '^-Toolchain=(.*)$') {
        $toolchainSpec = $Matches[1]
    }
    elseif ($arg -match '^-Toolchain$') {
        if ($i + 1 -ge $args.Count) { throw '-Toolchain needs a value.' }
        $i++
        $toolchainSpec = [string]$args[$i]
    }
    elseif ($arg -match '^-SelectToolchain$') { $selectToolchain = $true }
    elseif ($arg -match '^-ListToolchains$')  { $listToolchains = $true }
    elseif ($arg -match '^-NoPause$')         { $noPause = $true }
    elseif ($arg -match '^-DryRun$')          { $dryRun = $true }
    elseif ($arg -match '^-(Help|help|\?)$')  { $showHelp = $true }
    elseif ($arg -match '^-Configuration=(.*)$') {
        $configuration = $Matches[1]
        $gotConfiguration = $true
    }
    elseif ($arg -match '^-Configuration$') {
        if ($i + 1 -ge $args.Count) { throw '-Configuration needs a value.' }
        $i++
        $configuration = [string]$args[$i]
        $gotConfiguration = $true
    }
    elseif ($arg -match '^-Platform=(.*)$') {
        $platform = $Matches[1]
        $gotPlatform = $true
    }
    elseif ($arg -match '^-Platform$') {
        if ($i + 1 -ge $args.Count) { throw '-Platform needs a value.' }
        $i++
        $platform = [string]$args[$i]
        $gotPlatform = $true
    }
    elseif ($arg -match '^-Project=(.*)$') {
        $hostProject = $Matches[1]
    }
    elseif ($arg -match '^-Project$') {
        if ($i + 1 -ge $args.Count) { throw '-Project needs a value.' }
        $i++
        $hostProject = [string]$args[$i]
    }
    elseif ($arg -match '^-Target=(.*)$') {
        $projectTarget = $Matches[1]
    }
    elseif ($arg -match '^-Target$') {
        if ($i + 1 -ge $args.Count) { throw '-Target needs a value.' }
        $i++
        $projectTarget = [string]$args[$i]
    }
    elseif ($arg -match '^-SingleFile=(.*)$') {
        $singleFile = $Matches[1]
    }
    elseif ($arg -match '^-SingleFile$') {
        if ($i + 1 -ge $args.Count) { throw '-SingleFile needs a path.' }
        $i++
        $singleFile = [string]$args[$i]
    }
    elseif ($arg.StartsWith('-')) {
        [void]$remaining.Add($arg)
    }
    else {
        if (-not $gotConfiguration) {
            $configuration = $arg
            $gotConfiguration = $true
        }
        elseif (-not $gotPlatform) {
            $platform = $arg
            $gotPlatform = $true
        }
        else {
            throw "Unexpected argument: $arg"
        }
    }
}

$script:NoPause = $noPause
$script:ProjectTargetOverride = $projectTarget

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
    }
    catch {
        return $null
    }
}

function Get-ToolsetLine([version]$Version) {
    if ($null -eq $Version -or $Version.Major -ne 14) { return '' }
    if ($Version.Minor -ge 30) { return 'v143' }
    if ($Version.Minor -ge 20) { return 'v142' }
    if ($Version.Minor -ge 10) { return 'v141' }
    return 'v140'
}

function Get-PeLinkerVersion([string]$Path) {
    # Read MajorLinkerVersion/MinorLinkerVersion from the PE optional header.
    # Doing it this way avoids depending on dumpbin and on its localised output.
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            $reader = New-Object IO.BinaryReader($stream)
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            $stream.Position = $peOffset + 4 + 20 + 2
            $major = $reader.ReadByte()
            $minor = $reader.ReadByte()
            return [version]("$major.$minor")
        }
        finally {
            $stream.Dispose()
        }
    }
    catch {
        return $null
    }
}

function Find-HostProject {
    $explicit = $script:hostProject
    if (-not $explicit -and $env:BPI_HOST_PROJECT) { $explicit = $env:BPI_HOST_PROJECT }

    if ($explicit) {
        if (Test-Path -LiteralPath $explicit -PathType Container) {
            $found = @(Get-ChildItem -LiteralPath $explicit -Filter '*.uproject' -File -ErrorAction SilentlyContinue)
            if ($found.Count -eq 1) { return $found[0].FullName }
            throw "-Project=$explicit is a directory without exactly one .uproject."
        }
        if (Test-Path -LiteralPath $explicit -PathType Leaf) { return (Resolve-Path -LiteralPath $explicit).Path }
        throw "-Project=$explicit does not exist."
    }

    $candidates = @(Get-ChildItem -LiteralPath $script:RepoRoot -Filter '*.uproject' -File -ErrorAction SilentlyContinue)
    $parent = Split-Path -Parent $script:RepoRoot
    if ($parent -and (Test-Path -LiteralPath $parent)) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue)) {
            $candidates += @(Get-ChildItem -LiteralPath $dir.FullName -Filter '*.uproject' -File -ErrorAction SilentlyContinue)
        }
    }

    if ($candidates.Count -eq 1) { return $candidates[0].FullName }
    if ($candidates.Count -gt 1) {
        string[] $paths = $candidates | ForEach-Object { $_.FullName }
        throw "More than one host .uproject found: $($paths -join ', '). Pass -Project=<path>."
    }
    return $null
}

function Resolve-ProjectTarget([string]$ProjectFile) {
    # The editor target is conventionally <Project>Editor, but not always (Lyra's is LyraEditor).
    if ($script:ProjectTargetOverride) { return $script:ProjectTargetOverride }

    $fallback = ([IO.Path]::GetFileNameWithoutExtension((Split-Path -Leaf $ProjectFile))) + 'Editor'
    $sourceDir = Join-Path (Split-Path -Parent $ProjectFile) 'Source'
    if (-not (Test-Path -LiteralPath $sourceDir)) { return $fallback }

    $targets = @(Get-ChildItem -LiteralPath $sourceDir -Filter '*.Target.cs' -File -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Name.Substring(0, $_.Name.Length - '.Target.cs'.Length) })
    if ($targets -contains $fallback) { return $fallback }

    $editors = @($targets | Where-Object { $_ -like '*Editor' })
    if ($editors.Count -eq 1) { return $editors[0] }
    return $fallback
}

function Resolve-Engine {
    $result = [pscustomobject]@{
        Root          = $null
        ProjectFile   = $null
        ProjectTarget = $null
        Version       = $null
        Label         = ''
        Config        = ''
        ToolsetLine   = ''
        LinkerVersion = $null
    }

    $projectFile = Find-HostProject
    if ($projectFile) {
        $result.ProjectFile = $projectFile
        $result.ProjectTarget = Resolve-ProjectTarget $projectFile
    }

    $root = $env:UE_ROOT
    if (-not $root -and $projectFile) {
        $doc = Read-JsonFile $projectFile
        if ($doc -and $doc.EngineAssociation) {
            $assoc = [string]$doc.EngineAssociation
            if ($assoc.StartsWith('{')) {
                $key = 'HKCU:\Software\Epic Games\Unreal Engine\Builds'
                $props = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
                if ($props -and $props.PSObject.Properties.Name -contains $assoc) {
                    $root = [string]$props.$assoc
                }
            }
            else {
                $key = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$assoc"
                $props = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
                if ($props -and $props.InstalledDirectory) {
                    $root = [string]$props.InstalledDirectory
                }
            }

            # Registry-free installs (the CI layout): <base>\UE_<version>, BPI_ENGINE_ROOT overrides base.
            if (-not $root) {
                $base = if ($env:BPI_ENGINE_ROOT) { $env:BPI_ENGINE_ROOT } else { 'D:\UnrealEngine' }
                $conventional = Join-Path $base ('UE_' + $assoc)
                if (Test-Path -LiteralPath $conventional -PathType Container) { $root = $conventional }
            }
        }
    }

    if ($root) {
        $root = ($root -replace '/', '\').TrimEnd('\')
        if (Test-Path -LiteralPath $root) { $result.Root = $root }
    }
    if (-not $result.Root) { return $result }

    $buildVersion = Read-JsonFile (Join-Path $result.Root 'Engine\Build\Build.version')
    if ($buildVersion) {
        $result.Version = [version]("$($buildVersion.MajorVersion).$($buildVersion.MinorVersion).$($buildVersion.PatchVersion)")
        $result.Label = "UE $($buildVersion.MajorVersion).$($buildVersion.MinorVersion).$($buildVersion.PatchVersion)"
    }

    $binDir = Join-Path $result.Root 'Engine\Binaries\Win64'
    if (Test-Path -LiteralPath $binDir) {
        if (Test-Path -LiteralPath (Join-Path $binDir 'UE4Editor-Win64-DebugGame.exe')) { $result.Config = 'DebugGame' }
        elseif (Test-Path -LiteralPath (Join-Path $binDir 'UnrealEditor-Win64-DebugGame.exe')) { $result.Config = 'DebugGame' }
        elseif (Test-Path -LiteralPath (Join-Path $binDir 'UE4Editor.exe')) { $result.Config = 'Development' }
        elseif (Test-Path -LiteralPath (Join-Path $binDir 'UnrealEditor.exe')) { $result.Config = 'Development' }

        foreach ($name in @('UE4Editor-Core.dll', 'UnrealEditor-Core.dll')) {
            $core = Join-Path $binDir $name
            if (-not (Test-Path -LiteralPath $core)) { continue }
            $linker = Get-PeLinkerVersion $core
            if ($linker) {
                $result.LinkerVersion = $linker
                $result.ToolsetLine = Get-ToolsetLine $linker
                break
            }
        }
    }

    if (-not $result.ToolsetLine -and $result.Version) {
        if ($result.Version.Major -ge 5) { $result.ToolsetLine = 'v143' }
        elseif ($result.Version.Minor -ge 20) { $result.ToolsetLine = 'v142' }
        else { $result.ToolsetLine = 'v141' }
    }

    return $result
}

function Get-Toolsets([string]$InstallPath) {
    $base = Join-Path $InstallPath 'VC\Tools\MSVC'
    if (-not (Test-Path -LiteralPath $base)) { return @() }

    $toolsets = New-Object System.Collections.ArrayList
    foreach ($dir in Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue) {
        $cl = Join-Path $dir.FullName 'bin\Hostx64\x64\cl.exe'
        if (-not (Test-Path -LiteralPath $cl)) {
            $cl = Join-Path $dir.FullName 'bin\Hostx86\x64\cl.exe'
        }
        if (-not (Test-Path -LiteralPath $cl)) { continue }

        $info = (Get-Item -LiteralPath $cl).VersionInfo
        if ($info.ProductMajorPart -eq 0) { continue }
        $productVersion = [version]("$($info.ProductMajorPart).$($info.ProductMinorPart).$($info.ProductBuildPart)")

        $folderVersion = $null
        try { $folderVersion = [version]$dir.Name } catch { $folderVersion = $productVersion }

        [void]$toolsets.Add([pscustomobject]@{
            Folder        = $dir.Name
            FolderVersion = $folderVersion
            UbtVersion    = $productVersion.ToString(3)
            Line          = Get-ToolsetLine $productVersion
            ClPath        = $cl
        })
    }

    return @($toolsets | Sort-Object -Property FolderVersion -Descending)
}

function Get-VsInstances {
    $instances = New-Object System.Collections.ArrayList
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

    if (Test-Path -LiteralPath $vswhere) {
        $json = & $vswhere @('-all', '-prerelease', '-products', '*', '-format', 'json') 2>$null
        if ($LASTEXITCODE -eq 0 -and $json) {
            # Join before ConvertFrom-Json: piping the individual lines makes
            # Windows PowerShell merge the array elements into one object.
            # ConvertFrom-Json emits its top-level array as a single pipeline
            # object, so assign first and enumerate the assignment.
            $jsonText = ($json -join "`n")
            $parsed = $jsonText | ConvertFrom-Json
            foreach ($instance in @($parsed)) {
                $path = [string]$instance.installationPath
                $vcvars = Join-Path $path 'VC\Auxiliary\Build\vcvarsall.bat'
                if (-not (Test-Path -LiteralPath $vcvars)) { continue }

                $versionText = [string]$instance.installationVersion
                $major = 0
                [void][int]::TryParse(($versionText -split '\.')[0], [ref]$major)
                if ($major -lt 15) { continue }

                $toolsets = @(Get-Toolsets $path)
                if ($toolsets.Count -eq 0) { continue }

                $id = [string]$instance.instanceId
                if (-not $id) { $id = $path }

                [void]$instances.Add([pscustomobject]@{
                    Id          = $id
                    Name        = [string]$instance.displayName
                    VersionText = $versionText
                    Path        = $path
                    Major       = $major
                    VcVars      = $vcvars
                    Toolsets    = $toolsets
                })
            }
        }
    }

    if ($instances.Count -eq 0) {
        foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
            if (-not $root) { continue }
            $vsRoot = Join-Path $root 'Microsoft Visual Studio'
            if (-not (Test-Path -LiteralPath $vsRoot)) { continue }

            foreach ($yearDir in Get-ChildItem -LiteralPath $vsRoot -Directory -ErrorAction SilentlyContinue) {
                $major = 0
                switch (($yearDir.Name -replace '[^0-9]', '')) {
                    '2017' { $major = 15 }
                    '2019' { $major = 16 }
                    '2022' { $major = 17 }
                }
                if ($major -lt 15) { continue }

                foreach ($editionDir in Get-ChildItem -LiteralPath $yearDir.FullName -Directory -ErrorAction SilentlyContinue) {
                    $vcvars = Join-Path $editionDir.FullName 'VC\Auxiliary\Build\vcvarsall.bat'
                    if (-not (Test-Path -LiteralPath $vcvars)) { continue }
                    $toolsets = @(Get-Toolsets $editionDir.FullName)
                    if ($toolsets.Count -eq 0) { continue }

                    [void]$instances.Add([pscustomobject]@{
                        Id          = $editionDir.FullName
                        Name        = "$($yearDir.Name) $($editionDir.Name)"
                        VersionText = $yearDir.Name
                        Path        = $editionDir.FullName
                        Major       = $major
                        VcVars      = $vcvars
                        Toolsets    = $toolsets
                    })
                }
            }
        }
    }

    return $instances
}

function Get-UbtCompilerFlag([int]$InstallMajor, $EngineVersion) {
    # UE5 knows about VS2022 (-2022).  UE4.25 only has -2017/-2019 and discovers
    # VS2022 installations as VisualStudio2019 (installation version >= 16).
    if ($EngineVersion -and $EngineVersion.Major -ge 5) {
        if ($InstallMajor -ge 17) { return '-2022' }
        if ($InstallMajor -ge 16) { return '-2019' }
        return '-2017'
    }
    if ($InstallMajor -ge 16) { return '-2019' }
    return '-2017'
}

function Test-ToolsetSupported([string]$Line, $EngineVersion) {
    if (-not $EngineVersion) { return $true }
    if ($EngineVersion.Major -ge 5) { return ($Line -eq 'v142' -or $Line -eq 'v143') }
    return ($Line -eq 'v141' -or $Line -eq 'v142')
}

function Get-AllToolchains($Instances, $Engine) {
    $all = New-Object System.Collections.ArrayList
    foreach ($instance in $Instances) {
        foreach ($toolset in $instance.Toolsets) {
            [void]$all.Add([pscustomobject]@{
                Instance      = $instance
                Folder        = $toolset.Folder
                FolderVersion = $toolset.FolderVersion
                UbtVersion    = $toolset.UbtVersion
                Line          = $toolset.Line
                ClPath        = $toolset.ClPath
                UbtFlag       = Get-UbtCompilerFlag $instance.Major $Engine.Version
                Supported     = Test-ToolsetSupported $toolset.Line $Engine.Version
                Match         = [bool]($Engine.ToolsetLine -and $toolset.Line -eq $Engine.ToolsetLine)
            })
        }
    }

    return @($all | Sort-Object -Property `
        @{ Expression = { $_.Instance.Name } }, `
        @{ Expression = { $_.FolderVersion }; Descending = $true })
}

function Read-ToolchainConfig {
    if (-not (Test-Path -LiteralPath $script:ToolchainCfg)) { return $null }
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $script:ToolchainCfg -ErrorAction SilentlyContinue) {
        if ($line -match '^([^=]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
    }
    if (-not $values.ContainsKey('ToolsetFolder')) { return $null }
    return [pscustomobject]@{
        InstallId     = [string]$values['InstallId']
        InstallPath   = [string]$values['InstallPath']
        InstallName   = [string]$values['InstallName']
        ToolsetFolder = [string]$values['ToolsetFolder']
    }
}

function Save-ToolchainConfig($Toolchain) {
    if (-not (Test-Path -LiteralPath $script:SavedDir)) {
        New-Item -ItemType Directory -Path $script:SavedDir | Out-Null
    }
    Set-Content -LiteralPath $script:ToolchainCfg -Encoding ASCII -Value @(
        "InstallId=$($Toolchain.Instance.Id)",
        "InstallPath=$($Toolchain.Instance.Path)",
        "InstallName=$($Toolchain.Instance.Name)",
        "ToolsetFolder=$($Toolchain.Folder)",
        "ToolsetVersion=$($Toolchain.UbtVersion)"
    )
}

function Select-AutoToolchain($All, $Engine) {
    if ($Engine.ToolsetLine) {
        $matching = @($All | Where-Object { $_.Line -eq $Engine.ToolsetLine })
        if ($matching.Count -gt 0) {
            return ($matching | Sort-Object -Property FolderVersion -Descending | Select-Object -First 1)
        }
    }

    $supported = @($All | Where-Object { $_.Supported })
    if ($supported.Count -gt 0) {
        return ($supported | Sort-Object -Property FolderVersion -Descending | Select-Object -First 1)
    }

    return ($All | Sort-Object -Property FolderVersion -Descending | Select-Object -First 1)
}

function Find-Toolchain($All, $Engine, [string]$Spec) {
    $spec = $Spec.Trim()

    if ($spec -ieq 'auto' -or $spec -ieq 'saved' -or $spec -eq '') {
        if ($spec -ine 'auto') {
            $saved = Read-ToolchainConfig
            if ($saved) {
                $hit = $All | Where-Object {
                    ($_.Instance.Id -eq $saved.InstallId -or $_.Instance.Path -ieq $saved.InstallPath) -and
                    $_.Folder -eq $saved.ToolsetFolder
                } | Select-Object -First 1
                if (-not $hit) {
                    $hit = $All | Where-Object { $_.Folder -eq $saved.ToolsetFolder } | Select-Object -First 1
                }
                if ($hit) { return $hit }
                Write-Host "[WARNING] Saved toolchain $($saved.ToolsetFolder) is no longer installed; auto-selecting." -ForegroundColor Yellow
            }
        }
        return (Select-AutoToolchain $All $Engine)
    }

    if ($spec.Contains('/')) {
        $parts = $spec -split '/', 2
        $hit = $All | Where-Object {
            ($_.Instance.Id -eq $parts[0] -or $_.Instance.Path -ieq $parts[0]) -and
            $_.Folder -eq $parts[1]
        } | Select-Object -First 1
        if ($hit) { return $hit }
        throw "Toolchain '$spec' was not found. Run -ListToolchains to see the choices."
    }

    $matches = @($All | Where-Object { $_.Folder -eq $spec })
    if ($matches.Count -eq 1) { return $matches[0] }
    if ($matches.Count -gt 1) {
        $hit = $matches | Where-Object { $_.Match } | Select-Object -First 1
        if (-not $hit) { $hit = $matches | Sort-Object -Property FolderVersion -Descending | Select-Object -First 1 }
        Write-Host "[WARNING] MSVC $spec is installed more than once; using $($hit.Instance.Name)." -ForegroundColor Yellow
        return $hit
    }

    $instances = @($All | Where-Object { $_.Instance.Id -eq $spec -or $_.Instance.Path -ieq $spec })
    if ($instances.Count -gt 0) {
        $hit = $instances | Where-Object { $_.Match } | Sort-Object -Property FolderVersion -Descending | Select-Object -First 1
        if (-not $hit) { $hit = $instances | Sort-Object -Property FolderVersion -Descending | Select-Object -First 1 }
        return $hit
    }

    throw "Toolchain '$spec' was not found. Run -ListToolchains to see the choices."
}

function Format-Toolchain($Toolchain) {
    $tags = @()
    if ($Toolchain.Match) { $tags += 'engine match' }
    if (-not $Toolchain.Supported) { $tags += 'unsupported by this engine' }
    $suffix = ''
    if ($tags.Count -gt 0) { $suffix = ' [' + ($tags -join ', ') + ']' }
    return "$($Toolchain.Instance.Name) $($Toolchain.Instance.VersionText) - MSVC $($Toolchain.Folder) ($($Toolchain.Line))$suffix"
}

function Get-EngineSummary($Engine) {
    if (-not $Engine -or -not $Engine.Root) { return 'not resolved' }
    $linker = ''
    if ($Engine.LinkerVersion) {
        $linker = " - built with MSVC $($Engine.LinkerVersion) ($($Engine.ToolsetLine))"
    }
    return "$($Engine.Root) [$($Engine.Label)]$linker"
}

function Write-ToolchainWarnings($Toolchain, $Engine) {
    if ($Toolchain.Match) {
        Write-Host '[*] Match:     selected toolset line matches the engine.' -ForegroundColor Green
    }
    elseif ($Engine -and $Engine.ToolsetLine) {
        Write-Host "[WARNING] Selected $($Toolchain.Line), but the engine was built with $($Engine.ToolsetLine)." -ForegroundColor Yellow
    }

    if (-not $Toolchain.Supported) {
        $engineName = if ($Engine -and $Engine.Label) { $Engine.Label } else { 'this engine' }
        Write-Host "[WARNING] $($Toolchain.Line) is not supported by $engineName UBT; the build may fail." -ForegroundColor Yellow
    }
}

function Write-ToolchainSummary($Toolchain, $Engine) {
    Write-Host "[*] Engine:    $(Get-EngineSummary $Engine)"
    Write-Host "[*] Toolchain: $($Toolchain.Instance.Name) $($Toolchain.Instance.VersionText)"
    Write-Host "               MSVC $($Toolchain.Folder) ($($Toolchain.Line)), UBT pin $($Toolchain.UbtVersion), $($Toolchain.UbtFlag)"
    Write-ToolchainWarnings $Toolchain $Engine
}

function Show-ToolchainList($All, $Engine) {
    Write-Host "[*] Engine: $(Get-EngineSummary $Engine)"
    Write-Host ''
    if ($All.Count -eq 0) {
        Write-Host '[ERROR] No Visual Studio C++ toolset was found.' -ForegroundColor Red
        return
    }
    Write-Host 'Detected toolsets:'
    foreach ($toolchain in $All) {
        Write-Host "  $(Format-Toolchain $toolchain)"
    }
    $auto = Select-AutoToolchain $All $Engine
    Write-Host ''
    Write-Host "Auto would select: MSVC $($auto.Folder) ($($auto.Instance.Name))" -ForegroundColor Green
}

function Show-ToolchainMenu($All, $Engine) {
    if ($All.Count -eq 0) {
        Write-Host '[ERROR] No Visual Studio C++ toolset was found.' -ForegroundColor Red
        return $null
    }

    $auto = Select-AutoToolchain $All $Engine

    while ($true) {
        Write-Host ''
        Write-Host "[*] Engine: $(Get-EngineSummary $Engine)"
        Write-Host ''
        Write-Host 'Available Visual Studio / MSVC toolsets:'
        $map = @{}
        $index = 1
        foreach ($toolchain in $All) {
            $map[$index] = $toolchain
            Write-Host ("  [{0}] {1}" -f $index, (Format-Toolchain $toolchain))
            $index++
        }
        Write-Host ("  [A] Auto (engine match: MSVC {0})" -f $auto.Folder)
        Write-Host '  [Q] Cancel'
        $choice = Read-Host 'Select toolchain'

        if ($choice -match '^\d+$') {
            $n = [int]$choice
            if ($map.ContainsKey($n)) {
                $selected = $map[$n]
                Write-Host ''
                Write-ToolchainSummary $selected $Engine
                return $selected
            }
        }
        elseif ($choice -ieq 'a') {
            Write-Host ''
            Write-ToolchainSummary $auto $Engine
            return $auto
        }
        elseif ($choice -ieq 'q') {
            return $null
        }

        Write-Host 'Invalid selection.' -ForegroundColor Yellow
    }
}

function Get-EngineLibLine($Engine) {
    if ($Engine.Version -and $Engine.Version.Major -ge 5) { return 'v143' }
    return 'v142'
}

function Assert-PrebuiltLibraries($Engine) {
    $line = Get-EngineLibLine $Engine
    $dir = Join-Path $script:ThirdPartyDir ('lib' + [IO.Path]::DirectorySeparatorChar + $line)
    foreach ($name in @('cppdap.lib', 'detours.lib')) {
        $path = Join-Path $dir $name
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Missing $path. The prebuilt libraries are tracked per MSVC line; add the pair for $line."
        }
    }
}

function Get-BuildConfigurationPath {
    $appData = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)
    if (-not $appData) { $appData = $env:APPDATA }
    if (-not $appData) { throw 'Could not locate the per-user AppData folder for BuildConfiguration.xml.' }
    return Join-Path $appData 'Unreal Engine\UnrealBuildTool\BuildConfiguration.xml'
}

class BuildConfigurationScope {
    [string] $Path
    [bool] $Existed
    [byte[]] $Original
    hidden [string] $Backup
    hidden [string] $State
    [bool] $Touched

    BuildConfigurationScope([string] $InPath) {
        $this.Path = $InPath
        $this.Existed = Test-Path -LiteralPath $InPath
        if ($this.Existed) {
            $this.Original = [IO.File]::ReadAllBytes($InPath)
        }
        $this.Backup = "$InPath.bpi-backup"
        $this.State = "$InPath.bpi-state"
    }

    [bool] RepairStale() {
        if (-not (Test-Path -LiteralPath $this.State)) { return $false }
        $stateValue = Get-Content -LiteralPath $this.State -Raw -ErrorAction SilentlyContinue
        if ($stateValue) { $stateValue = $stateValue.Trim() }
        if ($stateValue -eq '1' -and (Test-Path -LiteralPath $this.Backup)) {
            Copy-Item -LiteralPath $this.Backup -Destination $this.Path -Force
        }
        elseif ($stateValue -eq '0') {
            Remove-Item -LiteralPath $this.Path -Force -ErrorAction SilentlyContinue
        }
        $this.ClearRecovery()
        return $true
    }

    [void] SaveForRecovery() {
        if ($this.Existed) {
            Copy-Item -LiteralPath $this.Path -Destination $this.Backup -Force
            Set-Content -LiteralPath $this.State -Encoding ASCII -Value '1'
        }
        else {
            Remove-Item -LiteralPath $this.Backup -Force -ErrorAction SilentlyContinue
            Set-Content -LiteralPath $this.State -Encoding ASCII -Value '0'
        }
        $this.Touched = $true
    }

    [void] RestoreAndClear() {
        if ($this.Existed) {
            [IO.File]::WriteAllBytes($this.Path, $this.Original)
        }
        else {
            Remove-Item -LiteralPath $this.Path -Force -ErrorAction SilentlyContinue
        }
        $this.ClearRecovery()
    }

    [void] ClearRecovery() {
        Remove-Item -LiteralPath $this.Backup -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $this.State -Force -ErrorAction SilentlyContinue
    }
}

function Set-BuildConfigurationCompilerVersion([string]$Path, [string]$Version) {
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory | Out-Null
    }

    $doc = $null
    if (Test-Path -LiteralPath $Path) {
        try {
            $doc = New-Object System.Xml.XmlDocument
            $doc.Load($Path)
        }
        catch {
            $doc = $null
        }
    }

    if (-not $doc) {
        $xml = '<?xml version="1.0" encoding="utf-8"?>' + "`r`n" +
               '<Configuration xmlns="https://www.unrealengine.com/BuildConfiguration">' + "`r`n" +
               '  <WindowsPlatform>' + "`r`n" +
               "    <CompilerVersion>$Version</CompilerVersion>`r`n" +
               '  </WindowsPlatform>' + "`r`n" +
               '</Configuration>' + "`r`n"
        Set-Content -LiteralPath $Path -Value $xml -Encoding UTF8
        return
    }

    $root = $doc.DocumentElement
    $namespace = $root.NamespaceURI
    if (-not $namespace) { $namespace = 'https://www.unrealengine.com/BuildConfiguration' }

    $windowsPlatform = $null
    foreach ($child in @($root.ChildNodes)) {
        if ($child.LocalName -ne 'WindowsPlatform') { continue }
        if (-not $windowsPlatform) { $windowsPlatform = $child }
        foreach ($existing in @($child.ChildNodes)) {
            if ($existing.LocalName -eq 'CompilerVersion') {
                [void]$child.RemoveChild($existing)
            }
        }
    }

    if (-not $windowsPlatform) {
        $windowsPlatform = $doc.CreateElement('WindowsPlatform', $namespace)
        [void]$root.AppendChild($windowsPlatform)
    }

    $compilerVersion = $doc.CreateElement('CompilerVersion', $namespace)
    $compilerVersion.InnerText = $Version
    [void]$windowsPlatform.AppendChild($compilerVersion)
    $doc.Save($Path)
}

function Sync-EngineBuildRules([string]$EngineRoot, $EngineVersion) {
    # UBT compiles the project's .Build.cs/.Target.cs into Intermediate/Build/BuildRules. A 5.x
    # rules assembly leaves a PDB that 4.25's bundled compiler cannot overwrite ("Unspecified error"
    # creating the project's ModuleRules PDB), so clear it when the engine changes.
    $key = '{0}|{1}' -f $EngineRoot, $EngineVersion
    $previous = ''
    if (Test-Path -LiteralPath $script:EngineStamp) {
        $previous = (Get-Content -LiteralPath $script:EngineStamp -Raw -ErrorAction SilentlyContinue)
        if ($previous) { $previous = $previous.Trim() }
    }
    if ($previous -eq $key) { return }

    $rulesDir = Join-Path $script:RepoRoot 'Intermediate\Build\BuildRules'
    if (Test-Path -LiteralPath $rulesDir) {
        Remove-Item -LiteralPath $rulesDir -Recurse -Force
        Write-Host "[*] Engine changed; cleared the stale rules assembly in $rulesDir."
    }
    if (-not (Test-Path -LiteralPath $script:SavedDir)) {
        New-Item -ItemType Directory -Path $script:SavedDir | Out-Null
    }
    Set-Content -LiteralPath $script:EngineStamp -Encoding ASCII -Value $key
}

function Get-FixtureEngineTag($Engine) {
    if (-not $Engine.Version) { return $null }
    return ('UE{0}.{1}' -f $Engine.Version.Major, $Engine.Version.Minor)
}

function Sync-Fixtures($Engine) {
    # Fixture packages are engine-specific: a .uasset saved by a newer engine cannot be loaded by an
    # older one. Keep one snapshot per engine under Fixtures/ and copy the matching set into the plugin's
    # normal content root, so all package paths (and the tests that embed them) stay unchanged.
    $tag = Get-FixtureEngineTag $Engine
    if (-not $tag) {
        Write-Host '[WARNING] Could not determine the engine version; fixture assets were not synced.' -ForegroundColor Yellow
        return
    }

    $snapshot = Join-Path $script:FixturesDir $tag
    if (-not (Test-Path -LiteralPath $script:ActiveFixturesDir)) {
        New-Item -ItemType Directory -Path $script:ActiveFixturesDir | Out-Null
    }

    # Always clear the generated set first, so a failed/absent snapshot cannot leave another engine's
    # packages behind and produce a confusing package-version error.
    Get-ChildItem -LiteralPath $script:ActiveFixturesDir -Filter '*.uasset' -File -ErrorAction SilentlyContinue |
        Remove-Item -Force

    $assets = @(Get-ChildItem -LiteralPath $snapshot -Filter '*.uasset' -File -ErrorAction SilentlyContinue)
    if ($assets.Count -eq 0) {
        Write-Host "[WARNING] No fixture snapshot for $tag under $snapshot." -ForegroundColor Yellow
        Write-Host '          The fixture tests will report missing assets. Generate the fixture with this' -ForegroundColor Yellow
        Write-Host "          engine once, then run 'Build.bat -Build SaveFixtures' to record it." -ForegroundColor Yellow
        return
    }

    foreach ($asset in $assets) {
        Copy-Item -LiteralPath $asset.FullName -Destination (Join-Path $script:ActiveFixturesDir $asset.Name) -Force
    }
    Write-Host "[*] Fixtures: $tag ($($assets.Count) asset(s) from $snapshot)"
}

function Save-Fixtures($Engine) {
    $tag = Get-FixtureEngineTag $Engine
    if (-not $tag) { throw 'Could not determine the engine version.' }

    $assets = @(Get-ChildItem -LiteralPath $script:ActiveFixturesDir -Filter '*.uasset' -File -ErrorAction SilentlyContinue)
    if ($assets.Count -eq 0) {
        throw "No .uasset files in $script:ActiveFixturesDir. Generate the fixture with $tag before saving it."
    }

    $snapshot = Join-Path $script:FixturesDir $tag
    if (-not (Test-Path -LiteralPath $snapshot)) {
        New-Item -ItemType Directory -Path $snapshot | Out-Null
    }
    Get-ChildItem -LiteralPath $snapshot -Filter '*.uasset' -File -ErrorAction SilentlyContinue | Remove-Item -Force
    foreach ($asset in $assets) {
        Copy-Item -LiteralPath $asset.FullName -Destination (Join-Path $snapshot $asset.Name) -Force
    }
    Write-Host "[SUCCESS] Saved $($assets.Count) fixture asset(s) for $tag to $snapshot"
}

function Get-PluginVersion() {
    $descriptor = Read-JsonFile (Join-Path $script:PluginDir 'BlueprintInspector.uplugin')
    if ($descriptor -and $descriptor.VersionName) { return [string]$descriptor.VersionName }
    if ($descriptor -and $null -ne $descriptor.Version) { return "v$($descriptor.Version)" }
    return 'v0'
}

# Stages the plugin as a self-contained folder and zips it, so a consumer unzips it into
# <Project>\Plugins and never needs the host project, the build scripts or the third-party sources.
function New-Package($Engine) {
    $tag = Get-FixtureEngineTag $Engine
    if (-not $tag) { throw 'Could not determine the engine version.' }
    Sync-Fixtures $Engine

    $sourceRoot = Join-Path $script:PluginDir 'Source'
    $prebuiltLine = Get-EngineLibLine $Engine
    $prebuiltDir = Join-Path $sourceRoot ('ThirdParty\lib\' + $prebuiltLine)
    $cppdapLib = Join-Path $prebuiltDir 'cppdap.lib'
    $detoursLib = Join-Path $prebuiltDir 'detours.lib'
    foreach ($lib in @($cppdapLib, $detoursLib)) {
        if (-not (Test-Path -LiteralPath $lib)) {
            throw "Missing $lib. The prebuilt libraries are tracked per MSVC line; add the pair for $prebuiltLine."
        }
    }

    $stagingRoot = Join-Path $script:RepoRoot 'Saved\Package'
    $staging = Join-Path $stagingRoot 'BlueprintInspector'
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    New-Item -ItemType Directory -Path $staging | Out-Null

    Copy-Item -LiteralPath (Join-Path $script:PluginDir 'BlueprintInspector.uplugin') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $script:RepoRoot 'LICENSE') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $script:RepoRoot 'THIRD-PARTY-NOTICES.md') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $script:PluginDir 'Content') -Destination $staging -Recurse
    $destData = Join-Path $staging 'Data'
    New-Item -ItemType Directory -Path $destData | Out-Null
    Copy-Item -LiteralPath (Join-Path $script:PluginDir 'Data\BlueprintLayoutGuide.md') -Destination $destData

    $destSource = Join-Path $staging 'Source'
    New-Item -ItemType Directory -Path $destSource | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'BlueprintInspector') -Destination $destSource -Recurse
    $destThirdParty = Join-Path $destSource 'ThirdParty'
    $destThirdPartyInclude = Join-Path $destThirdParty 'include'
    $destThirdPartyLib = Join-Path $destThirdParty ('lib\' + (Get-EngineLibLine $Engine))
    New-Item -ItemType Directory -Path $destThirdPartyInclude -Force | Out-Null
    New-Item -ItemType Directory -Path $destThirdPartyLib -Force | Out-Null
    Copy-Item -Path (Join-Path $sourceRoot 'ThirdParty\include\*') -Destination $destThirdPartyInclude -Recurse
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'ThirdParty\LICENSE-cppdap.txt') -Destination $destThirdParty
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'ThirdParty\LICENSE-detours.md') -Destination $destThirdParty
    Copy-Item -LiteralPath $cppdapLib -Destination $destThirdPartyLib
    Copy-Item -LiteralPath $detoursLib -Destination $destThirdPartyLib

    $destExtensionRoot = Join-Path $staging 'VSCodeExtension'
    New-Item -ItemType Directory -Path $destExtensionRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $script:PluginDir 'VSCodeExtension\README.md') -Destination $destExtensionRoot
    Copy-Item -LiteralPath (Join-Path $script:PluginDir 'VSCodeExtension\Launch') -Destination $destExtensionRoot -Recurse
    $destExtension = Join-Path $destExtensionRoot 'Extension'
    New-Item -ItemType Directory -Path $destExtension | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $script:PluginDir 'VSCodeExtension\Extension') -Force |
        Where-Object { $_.Name -notin @('node_modules', 'out') } |
        Copy-Item -Destination $destExtension -Recurse -Force

    $install = @"
# BlueprintInspector $tag

DAP debugger for Unreal Engine Blueprints (Win64). Copy this `BlueprintInspector` folder into
`<YourProject>\Plugins\BlueprintInspector`, keep the folder name, and open the project. The plugin
builds from source on first launch; UE 4.25 through 5.8 are supported.

The module links a prebuilt cppdap and Detours, so no CMake step is needed. `Content/Tests/OpcodeShowcase`
and `Data/BlueprintLayoutGuide.md` are the automation fixture set; they only matter if you run the
`BlueprintInspector.*` automation tests. `THIRD-PARTY-NOTICES.md` lists the bundled libraries and
their licenses.

See the repository README for the debug configuration and the VS Code extension.
"@
    Set-Content -LiteralPath (Join-Path $staging 'INSTALL.md') -Value $install -Encoding ASCII

    $version = Get-PluginVersion
    $hash = ''
    try { $hash = ((& git -C $script:RepoRoot rev-parse --short HEAD 2>$null) | Out-String).Trim() } catch { }
    $name = if ($hash) { "BlueprintInspector-$version-$hash.zip" } else { "BlueprintInspector-$version.zip" }
    $zip = Join-Path $script:RepoRoot (Join-Path 'Saved' $name)
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -LiteralPath $staging -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "[SUCCESS] Package: $zip" -ForegroundColor Green
}

function Invoke-PluginBuild($Toolchain, $Engine) {
    if (-not $Engine.Root) {
        throw 'Could not resolve the Unreal Engine root. Set UE_ROOT or check EngineAssociation in the .uproject.'
    }
    if (-not $Engine.ProjectFile) {
        throw 'No host .uproject found. Pass -Project=<path to a .uproject> or set BPI_HOST_PROJECT.'
    }

    if (-not $dryRun) {
        Sync-EngineBuildRules $Engine.Root $Engine.Version
        Sync-Fixtures $Engine
    }

    $buildBat = Join-Path $Engine.Root 'Engine\Build\BatchFiles\Build.bat'
    if (-not (Test-Path -LiteralPath $buildBat)) {
        throw "Build.bat not found under $($Engine.Root)."
    }

    Write-Host ''
    Write-Host '=== BlueprintInspector plugin ==='
    Write-ToolchainWarnings $Toolchain $Engine

    Assert-PrebuiltLibraries $Engine

    if ($Engine.Config -and $configuration -ne $Engine.Config) {
        Write-Host "[WARNING] The engine has a $($Engine.Config) editor build; $configuration may make UBT rebuild engine modules too." -ForegroundColor Yellow
    }

    $editors = Get-Process -Name 'UnrealEditor', 'UE4Editor' -ErrorAction SilentlyContinue
    if ($editors) {
        Write-Host '[WARNING] An editor is running; linking may fail on the loaded DLL. Close it, or use Live Coding (Ctrl+Alt+F11).' -ForegroundColor Yellow
    }

    # UE 4.25's UBT has no CppStandardVersion.Cpp20.  This plugin asks for C++20;
    # warn rather than letting the rules assembly fail with a confusing error.
    if ($Engine.Version -and $Engine.Version.Major -lt 5) {
        $rules = Join-Path $script:RepoRoot 'Source\BlueprintInspector\BlueprintInspector.Build.cs'
        if ((Test-Path -LiteralPath $rules) -and (Select-String -LiteralPath $rules -Pattern 'CppStandardVersion\.Cpp20' -Quiet)) {
            Write-Host "[WARNING] $($Engine.Label) UBT has no CppStandardVersion.Cpp20; the plugin rules may fail to compile." -ForegroundColor Yellow
        }
    }

    $configPath = Get-BuildConfigurationPath
    $configScope = [BuildConfigurationScope]::new($configPath)
    if ($configScope.RepairStale()) {
        Write-Host '[*] Repaired BuildConfiguration.xml left behind by an interrupted build.'
    }

    # 4.27 and every 5.x accept -CompilerVersion on the UBT command line. Only 4.25 needs the
    # per-user XML override, which is machine-global and therefore written only when unavoidable.
    $useCompilerSwitch = $Engine.Version -and ($Engine.Version -ge [version]'4.27')

    $ubtArgs = @(
        $Engine.ProjectTarget,
        $platform,
        $configuration,
        "-Project=$($Engine.ProjectFile)",
        '-WaitMutex',
        $Toolchain.UbtFlag
    )
    if ($useCompilerSwitch) { $ubtArgs += "-CompilerVersion=$($Toolchain.UbtVersion)" }
    if ($remaining.Count -gt 0) { $ubtArgs += @($remaining) }

    if ($singleFile) {
        $path = $singleFile
        if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $script:RepoRoot $path }
        $path = [IO.Path]::GetFullPath($path)
        if (-not (Test-Path -LiteralPath $path)) {
            throw "-SingleFile path not found: $path"
        }
        $ubtArgs += "-SingleFile=$path"
    }

    $displayArgs = ($ubtArgs | ForEach-Object {
        if ($_ -match '\s') { '"{0}"' -f $_ } else { $_ }
    }) -join ' '

    Write-Host "[*] Engine:  $($Engine.Root)"
    Write-Host "[*] Project: $($Engine.ProjectFile)"
    Write-Host "[*] Target:  $($Engine.ProjectTarget)"
    Write-Host "[*] Toolset: MSVC $($Toolchain.Folder) ($($Toolchain.Line)), UBT pin $($Toolchain.UbtVersion)"
    Write-Host "[*] Build:   $displayArgs"
    if ($useCompilerSwitch) {
        Write-Host "[*] Toolset pin: -CompilerVersion=$($Toolchain.UbtVersion) (no per-user config change)"
    }
    else {
        Write-Host '[*] UBT config override (temporary, restored afterwards):'
        Write-Host "    $configPath"
        Write-Host "    <WindowsPlatform><CompilerVersion>$($Toolchain.UbtVersion)</CompilerVersion></WindowsPlatform>"
    }
    Write-Host ''

    if ($dryRun) {
        Write-Host '[*] Dry run - nothing was built; no toolset pin was written.'
        return
    }

    $exitCode = 0
    try {
        if (-not $useCompilerSwitch) {
            # UBT 4.25 reads the toolset version only from the XML config, not from -ini:Engine
            # (that only feeds [ConfigFile] fields).
            $configScope.SaveForRecovery()
            Set-BuildConfigurationCompilerVersion $configPath $Toolchain.UbtVersion
        }
        & $buildBat @ubtArgs
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($configScope.Touched) {
            $configScope.RestoreAndClear()
        }
    }

    if ($exitCode -ne 0) {
        $script:RequestedExitCode = $exitCode
        $hint = ''
        if ($exitCode -eq 8) {
            $hint = " The host project's editor target may not be '$($Engine.ProjectTarget)'; pass -Target=<name>."
        }
        throw "UnrealBuildTool failed with exit code $exitCode.$hint See $($Engine.Root)\Engine\Programs\UnrealBuildTool\Log.txt"
    }

    if ($singleFile) {
        Write-Host '[*] Single-file compile check; the DLL was not relinked.'
        return
    }

    $isClean = $false
    foreach ($entry in $remaining) {
        if ($entry -ieq '-Clean') { $isClean = $true }
    }
    if ($isClean) {
        Write-Host '[*] Clean requested; no DLL expected.'
        return
    }

    # Both engines' modules can coexist in Binaries/Win64; report only the one this engine loads.
    $editorPrefix = if ($Engine.Version -and $Engine.Version.Major -ge 5) { 'UnrealEditor-BlueprintInspector' } else { 'UE4Editor-BlueprintInspector' }
    $dlls = @(Get-ChildItem -LiteralPath $script:PluginBinDir -Filter "$editorPrefix*.dll" -File -ErrorAction SilentlyContinue |
        Sort-Object -Property LastWriteTime -Descending)
    if ($dlls.Count -eq 0) {
        $dlls = @(Get-ChildItem -LiteralPath $script:PluginBinDir -Filter '*BlueprintInspector*.dll' -File -ErrorAction SilentlyContinue |
            Sort-Object -Property LastWriteTime -Descending)
    }
    if ($dlls.Count -gt 0) {
        $dll = $dlls[0]
        Write-Host "[SUCCESS] $($dll.Name)  $($dll.Length) bytes  $($dll.LastWriteTime)"
        Write-Host '[*] Restart the editor to load the rebuilt module.'
    }
    else {
        Write-Host "[WARNING] No BlueprintInspector DLL found in $script:PluginBinDir." -ForegroundColor Yellow
    }
}

function Show-Help {
@'
Build.ps1 - build the BlueprintInspector plugin and maintain its fixture snapshots.

Usage:
  Build.ps1 [-Build Plugin|Fixtures|SaveFixtures|Package] [options] [Configuration] [Platform] [UBT options...]

Entry points:
  Compile-Plugin.bat          -> Build.ps1 -Build Plugin
  Select-BuildToolchain.bat   -> Build.ps1 -SelectToolchain
  Build.bat                   -> Build.ps1 (interactive menu)

Fixture snapshots:
  Fixtures/<engine>/ holds the .uasset set for one engine (for example Fixtures/UE5.8).
  A plugin build copies the matching set into Content/Tests/OpcodeShowcase before UnrealBuildTool runs.
  -Build Fixtures      sync the active content folder for the selected engine only
  -Build SaveFixtures  record the current active .uasset set as this engine's snapshot
  -Build Package       stage the plugin as a self-contained folder and zip it for <Project>\Plugins

Options:
  -Toolchain=auto             ignore the saved choice and match the engine
  -Toolchain=saved            use Saved\BuildToolchain.cfg (default)
  -Toolchain=<version>        e.g. -Toolchain=14.29.30133
  -Toolchain=<id>/<version>   pin an exact Visual Studio instance
  -SelectToolchain            choose and save a toolset
  -ListToolchains             list detected toolsets and the automatic choice
  -NoPause                    do not wait for Enter at the end
  -DryRun                     print the commands without running them
  -Project=PATH               host .uproject (or its folder) to build the engine target through;
                              defaults to BPI_HOST_PROJECT, then a sibling project next to the checkout
  -Target=NAME                editor target to build; defaults to <Project>Editor, or the only
                              *Editor.Target.cs in the host project (Lyra uses LyraEditor)
  -SingleFile=PATH            compile one translation unit (plugin)
  -Configuration=NAME         default DebugGame
  -Platform=NAME              default Win64
  -Help                       show this text

Any other -Flag is forwarded to UnrealBuildTool (-Rebuild, -Clean, ...).

How the toolchain is chosen:
  The engine's PE linker version is read from Engine\Binaries\Win64\*Editor-Core.dll
  to determine which MSVC line built it (for example 14.24 -> v142).  Automatic
  selection picks the newest installed toolset in that line.  The exact cl.exe
  product version is pinned into UBT with -CompilerVersion on UE 4.27 and 5.x.
  UE 4.25 has no such switch, so only for that engine the version is written to
  the per-user BuildConfiguration.xml for the duration of the build, restored
  afterwards, and repaired on the next run if the build was killed.  That file
  applies to every Unreal project on the machine.

  The module links the prebuilt libraries under Source\ThirdParty\lib\<line>\.
'@ | Write-Host
}

$script:RequestedExitCode = 0
$exitCode = 0

try {
    if ($showHelp) {
        Show-Help
        $script:NoPause = $true
        return
    }

    if ([BuildConfigurationScope]::new((Get-BuildConfigurationPath)).RepairStale()) {
        Write-Host '[*] Repaired BuildConfiguration.xml left behind by an interrupted build.'
    }

    $engine = Resolve-Engine
    $instances = Get-VsInstances
    if ($instances.Count -eq 0) {
        throw 'No Visual Studio or Build Tools installation with the C++ toolset was found.'
    }

    $all = @(Get-AllToolchains $instances $engine)

    if ($listToolchains) {
        Show-ToolchainList $all $engine
        return
    }

    $toolchain = Find-Toolchain $all $engine $toolchainSpec

    if ($selectToolchain) {
        $selected = Show-ToolchainMenu $all $engine
        if (-not $selected) { return }
        $toolchain = $selected
        Save-ToolchainConfig $toolchain
        Write-Host ''
        Write-Host "[*] Saved MSVC $($toolchain.Folder) ($($toolchain.Instance.Name)) to $script:ToolchainCfg" -ForegroundColor Green
        if (-not $Build) { return }
    }

    if (-not $Build) {
        $hasBuildArgs = ($gotConfiguration -or $gotPlatform -or $singleFile -or $hostProject -or $remaining.Count -gt 0)
        if ($hasBuildArgs) {
            $Build = 'Plugin'
        }
        else {
            $done = $false
            while (-not $done) {
                Write-Host ''
                Write-ToolchainSummary $toolchain $engine
                Write-Host ''
                Write-Host 'What do you want to build?'
                Write-Host '  1) Plugin'
                Write-Host '  2) Change Visual Studio toolset'
                Write-Host '  3) Exit'
                $choice = Read-Host 'Choice'

                switch ($choice.Trim()) {
                    '1' { $Build = 'Plugin'; $done = $true }
                    '2' {
                        $selected = Show-ToolchainMenu $all $engine
                        if ($selected) {
                            $toolchain = $selected
                            Save-ToolchainConfig $toolchain
                            Write-Host "[*] Saved MSVC $($toolchain.Folder)." -ForegroundColor Green
                        }
                    }
                    '3' { $Build = 'Exit'; $done = $true }
                    default { Write-Host 'Invalid choice.' -ForegroundColor Yellow }
                }
            }
            if ($Build -eq 'Exit') { return }
        }
    }

    switch ($Build.ToLowerInvariant()) {
        'plugin'       { Invoke-PluginBuild $toolchain $engine }
        'fixtures'     { Sync-Fixtures $engine }
        'savefixtures' { Save-Fixtures $engine }
        'package'      { New-Package $engine }
        default { throw "Unknown build target '$Build'. Use Plugin, Fixtures, SaveFixtures, or Package." }
    }
}
catch {
    Write-Host ''
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
finally {
    if ($script:RequestedExitCode -ne 0) { $exitCode = $script:RequestedExitCode }
    if (-not $script:NoPause -and [Environment]::UserInteractive) {
        try { Read-Host 'Press Enter to continue' | Out-Null } catch { }
    }
    exit $exitCode
}
