<#
.SYNOPSIS
    Draws the extension icon: two Blueprint nodes joined by a wire with a breakpoint dot on it.

.DESCRIPTION
    The icon is generated rather than hand-drawn, so it can be regenerated or recoloured without a
    drawing tool. It writes a 128x128 PNG, which is the size the Marketplace shows and the extension
    manifest points at.

    The plate is a dark rounded square with a transparent surround, so the icon reads on light and dark
    backgrounds: the nodes are the engine's blue, the wire is neutral, and the breakpoint is the red VS
    Code uses for a set breakpoint.

.EXAMPLE
    ./Scripts/New-ExtensionIcon.ps1

.EXAMPLE
    ./Scripts/New-ExtensionIcon.ps1 -OutDir C:\Temp -NodeColor 62,147,242
#>

#Requires -Version 5.1

[CmdletBinding()]
param(
    [string]$OutDir,
    [string]$FileName = 'icon.png',
    [int[]]$NodeColor = @(0x3E, 0x93, 0xF2),
    [int[]]$BreakpointColor = @(0xE8, 0x4A, 0x4F)
)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is not populated while parameter defaults are evaluated, so resolve it here.
if (-not $OutDir) {
    $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'VSCodeExtension\Extension'
}

Add-Type -AssemblyName System.Drawing

function New-RoundedPath {
    param([single]$X, [single]$Y, [single]$W, [single]$H, [single]$R)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-Icon {
    param([int]$Size, [string]$FilePath)

    $scale = $Size / 128.0
    $bitmap = New-Object System.Drawing.Bitmap $Size, $Size
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $platePath = New-RoundedPath 0 0 $Size $Size (26 * $scale)
    $plateBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point 0, 0),
        (New-Object System.Drawing.Point $Size, $Size),
        [System.Drawing.Color]::FromArgb(255, 0x28, 0x31, 0x44),
        [System.Drawing.Color]::FromArgb(255, 0x12, 0x17, 0x21))
    $graphics.FillPath($plateBrush, $platePath)

    $nodeBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point 0, 0),
        (New-Object System.Drawing.Point 0, $Size),
        [System.Drawing.Color]::FromArgb(255, $NodeColor[0], $NodeColor[1], $NodeColor[2]),
        [System.Drawing.Color]::FromArgb(255, [int]($NodeColor[0] * 0.6), [int]($NodeColor[1] * 0.6), [int]($NodeColor[2] * 0.85)))
    $nodePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0x7A, 0xB4, 0xFF)), (2 * $scale)

    foreach ($node in @(@(13, 20, 52, 31), @(63, 77, 52, 31))) {
        $nodePath = New-RoundedPath ($node[0] * $scale) ($node[1] * $scale) ($node[2] * $scale) ($node[3] * $scale) (7 * $scale)
        $graphics.FillPath($nodeBrush, $nodePath)
        $graphics.DrawPath($nodePen, $nodePath)
    }

    $wirePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0xAF, 0xBA, 0xCB)), (5 * $scale)
    $wirePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $wirePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $wirePen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $wirePoints = [System.Drawing.Point[]]@(
        (New-Object System.Drawing.Point ([int](65 * $scale)), ([int](35 * $scale))),
        (New-Object System.Drawing.Point ([int](92 * $scale)), ([int](35 * $scale))),
        (New-Object System.Drawing.Point ([int](92 * $scale)), ([int](77 * $scale))))
    $graphics.DrawLines($wirePen, $wirePoints)

    $ringBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x12, 0x17, 0x21))
    $graphics.FillEllipse($ringBrush, (92 - 15) * $scale, (56 - 15) * $scale, 30 * $scale, 30 * $scale)
    $dotBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, $BreakpointColor[0], $BreakpointColor[1], $BreakpointColor[2]))
    $graphics.FillEllipse($dotBrush, (92 - 12) * $scale, (56 - 12) * $scale, 24 * $scale, 24 * $scale)

    $graphics.Dispose()

    if (-not (Test-Path -LiteralPath $OutDir)) {
        New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    }
    $target = Join-Path $OutDir $FileName
    $bitmap.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Host ("wrote {0} ({1}x{1})" -f $target, $Size)
}

New-Icon -Size 128 -FilePath (Join-Path $OutDir $FileName)
