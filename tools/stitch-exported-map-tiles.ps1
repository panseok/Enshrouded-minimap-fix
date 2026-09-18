param(
  [Parameter(Mandatory = $true)]
  [string]$TileDirectory,

  [int]$TileColumns = 10,
  [int]$TileRows = 10,

  [string]$Pattern = "ui_map_part_*.png",

  [string]$OutputPng = "",

  [switch]$SkipFirst,

  [int]$StartPart = -1,

  [int]$Step = 1
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

if (-not (Test-Path -LiteralPath $TileDirectory)) {
  throw "Tile directory not found: $TileDirectory"
}

$tiles = Get-ChildItem -LiteralPath $TileDirectory -Filter $Pattern

if ($StartPart -ge 0) {
  $tiles = $tiles | Where-Object {
    if ($_.BaseName -match 'ui_map_part_(\d+)$') {
      $part = [int]$Matches[1]
      return $part -ge $StartPart -and (($part - $StartPart) % $Step) -eq 0
    }
    return $false
  } | Sort-Object {
    [int]([regex]::Match($_.BaseName, '(\d+)$').Groups[1].Value)
  }
}
else {
  $tiles = $tiles | Sort-Object Name
  if ($SkipFirst -and $tiles.Count -gt 0) {
    $tiles = $tiles | Select-Object -Skip 1
  }
}

$needed = $TileColumns * $TileRows
if ($tiles.Count -lt $needed) {
  throw "Need at least $needed tile PNGs, found $($tiles.Count) in $TileDirectory"
}

$tiles = $tiles | Select-Object -First $needed
$first = [System.Drawing.Image]::FromFile($tiles[0].FullName)
$tileWidth = $first.Width
$tileHeight = $first.Height
$first.Dispose()

if (-not $OutputPng) {
  $OutputPng = Join-Path $TileDirectory "embervale_map_stitched.png"
}

$canvas = New-Object System.Drawing.Bitmap ($tileWidth * $TileColumns), ($tileHeight * $TileRows), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($canvas)
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half

for ($index = 0; $index -lt $needed; $index++) {
  $tile = [System.Drawing.Image]::FromFile($tiles[$index].FullName)
  try {
    $x = ($index % $TileColumns) * $tileWidth
    $y = [Math]::Floor($index / $TileColumns) * $tileHeight
    $graphics.DrawImage($tile, $x, $y, $tileWidth, $tileHeight)
  }
  finally {
    $tile.Dispose()
  }
}

$graphics.Dispose()

# Sanity-check the stitched result before writing it out. A handful of the
# game's UiTextureResource candidates (e.g. detail-height / LOD data) decode
# structurally fine but contain a single repeated color -- usually because
# image.decode_texture in the exporter mod does not support that pixel
# format, or the game had not streamed real data into that mip yet. Catching
# it here saves a trip through convert-exported-map.ps1 to discover the
# "map" is just one flat color.
$sampleColors = New-Object System.Collections.Generic.HashSet[string]
$sampleCols = 24
$sampleRows = 24
for ($sy = 0; $sy -lt $sampleRows; $sy++) {
  for ($sx = 0; $sx -lt $sampleCols; $sx++) {
    $px = [Math]::Min([int](($sx + 0.5) * $canvas.Width / $sampleCols), $canvas.Width - 1)
    $py = [Math]::Min([int](($sy + 0.5) * $canvas.Height / $sampleRows), $canvas.Height - 1)
    $c = $canvas.GetPixel($px, $py)
    [void]$sampleColors.Add("$($c.R),$($c.G),$($c.B),$($c.A)")
    if ($sampleColors.Count -gt 1) { break }
  }
  if ($sampleColors.Count -gt 1) { break }
}

if ($sampleColors.Count -le 1) {
  Write-Warning "Stitched image appears to be a single solid color across all sampled pixels. The source tiles are likely blank/placeholder data (see export-log.txt for a matching WARNING from the exporter mod). Consider trying a different tile pattern or catalog candidate before converting this into the minimap asset."
}

$canvas.Save($OutputPng, [System.Drawing.Imaging.ImageFormat]::Png)
$canvas.Dispose()

Write-Host "Wrote $OutputPng ($($tileWidth * $TileColumns) x $($tileHeight * $TileRows)) from $needed tiles"
