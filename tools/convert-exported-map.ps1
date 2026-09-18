param(
  [Parameter(Mandatory = $true)]
  [string]$InputPng,

  [ValidateSet(512, 768, 1024, 1280, 2048)]
  [int]$Size = 1280,

  [string]$OutputRgba = "",

  [switch]$BackupExisting
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $OutputRgba) {
  $OutputRgba = Join-Path $root "assets\embervale_realmap_$Size.rgba"
}

if (-not (Test-Path -LiteralPath $InputPng)) {
  throw "Input PNG not found: $InputPng"
}

$outDir = Split-Path -Parent $OutputRgba
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

if ($BackupExisting -and (Test-Path -LiteralPath $OutputRgba)) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  Copy-Item -LiteralPath $OutputRgba -Destination "$OutputRgba.backup-$stamp" -Force
}

$source = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $InputPng))

# Sanity-check the source PNG before spending time on a full per-pixel
# resample. Some exported map candidates (detail-height / unloaded LOD
# textures) decode to a single solid color; converting one of those just
# produces a flat-colored minimap asset with no useful data.
$sourceBitmapForCheck = New-Object System.Drawing.Bitmap($source)
$sampleColors = New-Object System.Collections.Generic.HashSet[string]
$sampleCols = 24
$sampleRows = 24
for ($sy = 0; $sy -lt $sampleRows; $sy++) {
  for ($sx = 0; $sx -lt $sampleCols; $sx++) {
    $px = [Math]::Min([int](($sx + 0.5) * $sourceBitmapForCheck.Width / $sampleCols), $sourceBitmapForCheck.Width - 1)
    $py = [Math]::Min([int](($sy + 0.5) * $sourceBitmapForCheck.Height / $sampleRows), $sourceBitmapForCheck.Height - 1)
    $c = $sourceBitmapForCheck.GetPixel($px, $py)
    [void]$sampleColors.Add("$($c.R),$($c.G),$($c.B),$($c.A)")
    if ($sampleColors.Count -gt 1) { break }
  }
  if ($sampleColors.Count -gt 1) { break }
}
$sourceBitmapForCheck.Dispose()

if ($sampleColors.Count -le 1) {
  Write-Warning "Input PNG appears to be a single solid color across all sampled pixels. This usually means the exported texture was blank/placeholder data (check export-log.txt for a matching WARNING). Converting it will still produce a flat-colored .rgba, not a real map."
}

$bitmap = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$graphics.DrawImage($source, 0, 0, $Size, $Size)
$graphics.Dispose()
$source.Dispose()

$bytes = New-Object byte[] ($Size * $Size * 4)
$i = 0
for ($y = 0; $y -lt $Size; $y++) {
  for ($x = 0; $x -lt $Size; $x++) {
    $color = $bitmap.GetPixel($x, $y)
    $bytes[$i++] = $color.R
    $bytes[$i++] = $color.G
    $bytes[$i++] = $color.B
    $bytes[$i++] = $color.A
  }
}
$bitmap.Dispose()

[System.IO.File]::WriteAllBytes($OutputRgba, $bytes)
Write-Host "Wrote $OutputRgba ($Size x $Size raw RGBA)"
