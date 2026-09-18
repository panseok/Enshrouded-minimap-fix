# Minimap Fog Zone Exporter (EML)

One-shot export of the shroud (FogZone) layer of the in-game map, used by
`tools/map-render/build_hd_map.py --fog-dir` to draw shroud areas on the minimap.

1. Install EML (`dbghelp.dll` from https://github.com/Brabb3l/kfc-parser/releases)
   into the Enshrouded folder.
2. Copy this folder to `<Enshrouded>/mods/eml-fog-exporter`.
3. In `<Enshrouded>/eml.json` set `"use_export_flag": true`.
4. Start the game once (the main menu is enough), then quit.
5. The files land in `<Enshrouded>/export/minimap_fog_exporter/`.

EML and this folder can be removed afterwards.
