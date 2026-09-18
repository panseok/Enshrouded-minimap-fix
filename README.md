# Enshrouded Minimap

Integrated in-game minimap mod for Enshrouded using Shroudtopia.

This is not an external Windows overlay. The mod hooks into the game client and
draws the minimap directly inside the Enshrouded frame.

## What It Does

- Shows a circular minimap on the right side of the screen.
- Defaults to the bottom-right corner and supports top-right, middle-right,
  and bottom-right placement.
- Uses a premium compass-style frame asset.
- Renders the real Embervale map at minimap scale.
- Shows the player's position and facing direction.
- Uses real map marker icons extracted from the game's map UI.
- Shows nearby points of interest that are visible or detected by the map.
- Uses fog-of-war and POI data as a fallback when the game does not expose all
  live markers.
- Renders inside the game's Vulkan frame without a separate overlay window.

## Controls

- `+` zooms in.
- `-` zooms out.
- `F10` toggles the minimap on/off without leaving the game.

The numpad `+`, `-`, and `*` keys also work.

## Download

Download the latest zip from the release page:

[Enshrouded Minimap v0.4.46](https://github.com/elxokker/Enshrouded-minimap/releases/tag/v0.4.46)

Release asset:

`enshrouded-minimap-v0.4.46.zip`

## Installing the Zip

1. Close Enshrouded.
2. Download `enshrouded-minimap-v0.4.46.zip`.
3. Extract the zip.
4. Copy the full `minimap_mod` folder to:

```text
C:\Program Files (x86)\Steam\steamapps\common\Enshrouded\mods\minimap_mod
```

The final folder should look like this:

```text
Enshrouded
+-- mods
    +-- minimap_mod
        +-- minimap_mod.dll
        +-- mod.json
        +-- assets
            +-- embervale_minimap_frame.rgba
            +-- embervale_minimap_icons.bin
            +-- embervale_realmap_hd_0_0.rgba ... embervale_realmap_hd_3_3.rgba
            +-- embervale_shroud_sdf.r8
            +-- embervale_*.spv
            +-- ...
```

5. Start Enshrouded with Shroudtopia.

If you already had an older version installed, replace the whole `minimap_mod`
folder with the new one.

## Position Config

The minimap position is controlled from Shroudtopia's config file:

```text
C:\Program Files (x86)\Steam\steamapps\common\Enshrouded\shroudtopia.json
```

Set `mods.minimap_mod.position` to one of these values:

- `top-right`
- `middle-right`
- `bottom-right`

Example:

```json
{
  "mods": {
    "minimap_mod": {
      "active": true,
      "position": "bottom-right",
      "toggle_key": "F10",
      "render_camera_fallback": true,
      "debug_logging": false,
      "map_renderer": "gpu",
      "map_follow": "center",
      "map_sample_step": 2,
      "max_icons": 64
    }
  }
}
```

The mod also reads `mods.minimap_mod.toggle_key` every second while active.
Recommended value: `F10`. Supported readable values include `F1`-`F24`,
`insert`, `delete`, `home`, `end`, `pageup`, `pagedown`, `backspace`, and
`numpad-*`.

The minimap is always north-up: the map, compass frame and markers never
rotate, and the player arrow turns with the camera.

`map_follow` (default `center`): `center` keeps the player arrow in the middle
and scrolls the map. `static` keeps the map still and moves
the arrow across it; when the arrow gets close to the rim the view glides back
onto the player.

`render_camera_fallback` is enabled by default. It lets the minimap recover its
player position from render data when a game update stops the normal UI position
feed from firing before the minimap draws.

The mod reads these values directly and refreshes them every second while active.
If the minimap is not loaded yet, start or restart the game after changing it.

## Map Renderer

By default the map is drawn on the GPU: the map image is uploaded once as a
mipmapped Vulkan texture and the whole minimap disc is drawn by
`embervale_minimap_map.frag.spv` (rotation, zoom, trilinear filtering and an
anti-aliased rim). This replaces the old CPU path, which filled the disc with
2x2-pixel clear rects and 48-level color quantization and therefore looked
blurry and banded no matter how detailed the map image was.

- `map_renderer`: `gpu` (default) or `cpu` (old rasterizer, also used
  automatically if the GPU path cannot be created). `map_sample_step` and
  `map_light` only affect the `cpu` renderer.

The map image is loaded in this order:

1. `embervale_realmap_hd_<row>_<col>.rgba` - a square grid of equally sized
   square raw RGBA tiles (row 0 = north, col 0 = west). The shipped HD map is
   8192x8192 split into 4x4 tiles of 2048x2048, so no single file exceeds
   GitHub's size limit.
2. `embervale_realmap_hd.rgba`, `embervale_realmap_1280.rgba`,
   `embervale_realmap_1024.rgba`, `embervale_realmap_768.rgba`,
   `embervale_realmap_512.rgba` - single square raw RGBA files.

Any square size from 512 to 8192 works; the size is inferred from the bytes.

The HD map is rendered from the game's own map data (elevation textures and the
map shader's color/isoline gradients) exported with `tools/eml-map-exporter`:

```powershell
python tools\map-render\build_hd_map.py "D:\SteamLibrary\steamapps\common\Enshrouded\export\minimap_map_exporter" assets
```

The map shader is generated by `python tools\shaders\build_map_shader.py`
(no glslang needed; the GLSL equivalent is documented in that script).

### Square window

The minimap is a square window: the map shader, the CPU renderer, marker clipping
and the player arrow all use the square boundary. `embervale_minimap_frame.rgba`
is the square frame, generated from the original round frame with
`python tools\map-render\make_square_frame.py <round.rgba> <square.rgba>`.

### Icons and player marker

Map icons are the game's own map marker icons at full resolution (64/128 px),
drawn on the GPU from a mipmapped sprite atlas (`embervale_minimap_sprite.frag.spv`)
in their original colors. The player is a lime-green triangle pointing in the view
direction; other players are sky-blue dots and NPCs yellow dots, all generated at
runtime. Other players are recognized as moving world-map markers without a map
icon; with `debug_logging` on, the log prints a `master marker census` every 15 s. Marker
types that have no icon in the game's marker registry are not drawn (the world map
does not draw them either). Without the sprite shader the icons fall back to a
CPU rasterizer.

`embervale_minimap_icons.bin` is built from a one-time export with
`tools/eml-icon-exporter`:

```powershell
python tools\map-render\build_icon_atlas.py "D:\SteamLibrary\steamapps\common\Enshrouded\export\minimap_icon_exporter" assets\embervale_minimap_icons.bin --legacy <old embervale_minimap_icons.bin>
```

`--legacy` keeps the DLL's small built-in marker kinds pointing at the matching game
icons. The sprite shader is generated by `python tools\shaders\build_sprite_shader.py`.

### Shroud areas

Shroud zones are drawn on top of the map (light blue tint, light blue border
line). `embervale_shroud_sdf.r8` holds the signed distance to the shroud border
(4096x4096 uint8, row 0 = north, 128 = border, positive outside, 48 world units
per side). It is loaded into the map texture's alpha channel, so the GPU shader
keeps the border crisp at every zoom level; the CPU renderer draws it too. If the
file is missing the map is drawn without shroud.

The file is built from the game's `FogZone` map textures, exported once with
`tools/eml-fog-exporter`:

```powershell
python tools\map-render\build_shroud_sdf.py "D:\SteamLibrary\steamapps\common\Enshrouded\export\minimap_fog_exporter" assets
```

## EML Compatibility Notes

This minimap is a native Shroudtopia mod. It is not an EML-native Lua mod and
EML does not load `minimap_mod.dll` by itself.

The release includes a no-op `src/mod.lua` and EML-neutral manifest fields so
EML setups that scan every folder under `mods/` do not treat `minimap_mod` as an
incomplete Lua package.

For mixed EML + Shroudtopia setups:

- Keep Shroudtopia installed with `winmm.dll` and `shroudtopia.dll`.
- Keep this mod at `mods/minimap_mod`.
- If EML with `dbghelp.dll` crashes on client startup, try EML's `dinput8.dll`
  proxy instead of `dbghelp.dll`, as recommended by EML's own troubleshooting.
- Do not expect an EML-only setup to load the minimap DLL. Shroudtopia is still
  required.

## Building From Source

Requirements:

- Windows.
- Visual Studio with MSBuild and the C++ toolset.
- Shroudtopia installed in the game folder to load the mod.

Build:

```powershell
.\build-release.ps1
```

Install to the default Steam path:

```powershell
.\install.ps1
```

Install to a custom Enshrouded path:

```powershell
.\install.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Enshrouded"
```

## Repository Layout

- `src/` - mod source code.
- `include/` - minimal Shroudtopia headers required to build.
- `assets/` - runtime assets copied next to the DLL.
- `build-release.ps1` - builds `Release|x64`.
- `install.ps1` - installs the mod and backs up the previous install.

## Version

Current mod version: `0.4.46`.

The mod resolves hook signatures near the known Enshrouded client addresses at
load time, which makes small game updates less likely to break the minimap.
Large game updates can still require structure offsets to be revalidated.
