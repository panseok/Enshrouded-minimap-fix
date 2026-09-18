# In-Game Map Export

This folder contains a small EML exporter for pulling Enshrouded's in-game
Embervale map texture and converting it into the raw RGBA file used by this
Shroudtopia minimap.

## Export From The Game

1. Copy `tools/eml-map-exporter` into your Enshrouded `mods` folder.
2. Make sure EML has the `export` capability enabled.
3. Launch the game with EML once.
4. Check the EML export directory for:

```text
minimap_map_exporter/embervale_map_*.png
minimap_map_exporter/embervale_map_*.rgba
minimap_map_exporter/ui_map_part_*.png
minimap_map_exporter/ui_map_part_*.rgba
minimap_map_exporter/export-log.txt
```

The exporter first tries the known map resource GUID:

```text
01bcbd07-bdbf-41c0-9999-5d58fb1a3aa1
```

If the game's resource shape changed, inspect `export-log.txt` and the
generated `.cache/lua/types.lua` in the game folder.

Current game builds expose `keen::UiMapResource` as a 10x10 tiled map. If the
exporter produced `ui_map_part_*.png` files, stitch the first 100 map tiles:

```powershell
.\tools\stitch-exported-map-tiles.ps1 `
  -TileDirectory "C:\Path\To\Enshrouded\export\minimap_map_exporter" `
  -TileColumns 10 `
  -TileRows 10
```

If the first exported part is not a map tile, rerun with `-SkipFirst`.

## Convert PNG To Minimap RGBA

If the exporter produced or stitched a PNG, convert it into the file this mod
loads first:

```powershell
.\tools\convert-exported-map.ps1 `
  -InputPng "C:\Path\To\Enshrouded\export\minimap_map_exporter\embervale_map_whatever.png" `
  -Size 1280 `
  -BackupExisting
```

By default this writes:

```text
assets/embervale_realmap_1280.rgba
```

Then rebuild or reinstall the minimap mod.

## Blank/Solid-Color Exports

Not every texture the catalog scan calls a "map-like candidate" is actually
usable. In particular, height/detail-style resources have decoded to a
single repeated byte (fully black, fully transparent) in testing --
structurally valid PNG/RGBA files, right dimensions, but zero real content.
That most likely means either the game had not streamed real data into that
mip/LOD yet, or `image.decode_texture` in `src/mod.lua` does not support
that texture's pixel format and silently zero-filled the output.

The exporter (`0.1.4-blank-detect` and later), `stitch-exported-map-tiles.ps1`,
and `convert-exported-map.ps1` all now sample the data at each stage and log
a `WARNING` (in `export-log.txt` for the exporter, on the console for the
PowerShell scripts) when everything sampled comes back as one solid color.
If you see that warning:

- Do not bother stitching/converting that candidate further -- it will only
  produce a flat-colored `.rgba`.
- Check `export-log.txt` for the `format=` value logged next to the
  candidate. If the same format keeps coming back blank across runs, that
  format is probably unsupported by `image.decode_texture` and needs
  special-casing in `mod.lua`, not a different export attempt.
- Try a different candidate from `texture-catalog.tsv` / the
  `catalog map-like candidate #N` log lines instead -- there are usually
  several GUID/part combinations that match the "map-like" keyword filter,
  and only some of them hold real pixel data at any given time.
