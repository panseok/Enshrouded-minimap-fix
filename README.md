# Enshrouded Minimap

Integrated in-game minimap mod for Enshrouded using Shroudtopia.

This is not an external Windows overlay. The mod hooks into the game client and
draws the minimap directly inside the Enshrouded frame.

## What It Does

- Shows a square, north-up minimap in the top-right corner of the screen.
- Press `Esc` and drag it anywhere; drag its bottom-right corner to resize.
  The layout is saved and restored on the next start.
- Thin, semi-transparent frame with N / E / S / W letters.
- Renders the real Embervale map (8192x8192, drawn on the GPU) at minimap scale,
  with the shroud zones tinted and outlined.
- Covers the parts of the map you have not discovered yet with slate grey,
  straight from the game's own fog-of-war grid (the same data the world map
  uses), updated live as you explore. Map icons in undiscovered areas stay hidden.
- Your own marker: a lime triangle pointing in your direction of travel, or a
  lime dot with the view direction switched off (`F11`).
- Other players as sky-blue dots with their names underneath.
- Pings: the pinger's ping icon tinted green with their name under it. A ping
  stays until that player pings somewhere else.
- Waypoints: the game's waypoint icon, or, when the waypoint sits on another
  map icon, a yellow outline that follows that icon's shape.
- Pings and waypoints outside the minimap range are pinned to the rim.
- Map markers (chests, altars, dungeons, ...) with the game's own icons, taken
  from the same marker lists the world map draws, so only discovered markers
  appear.
- NPCs as the game's blue "person" icon.
- Renders inside the game's Vulkan frame without a separate overlay window.

## Controls

- `+` zooms in.
- `-` zooms out.
- `F10` turns the whole mod on/off. While it is off the game-thread hooks
  return immediately, the background trackers stop, nothing is recorded or
  submitted in the present hook, and the Vulkan objects (map texture, fog
  texture and sprite atlas) are released. Pressing it again rebuilds
  everything (about a second while the map reloads).
- `F11` turns the view direction on/off. Off: your own marker becomes a round
  lime dot. On: the lime triangle turns with you.
- `Esc` (game menu): drag the minimap to move it, drag its bottom-right corner
  to resize it.

The numpad `+`, `-`, and `*` keys also work (`*` toggles like `F10`).

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

## Config

Settings live in Shroudtopia's config file:

```text
C:\Program Files (x86)\Steam\steamapps\common\Enshrouded\shroudtopia.json
```

Example (every key is optional):

```json
{
  "mods": {
    "minimap_mod": {
      "active": true,
      "toggle_key": "F10",
      "heading_toggle_key": "F11",
      "debug_logging": false,
      "map_follow": "center",
      "max_icons": 64,
      "label_font_size": 17,
      "fog_strength": 100,
      "map_texture_size": 8192
    }
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `toggle_key` | `F10` | Turns the whole mod on/off. |
| `heading_toggle_key` | `F11` | Turns the view direction on/off. |
| `map_follow` | `center` | `center` keeps you in the middle and scrolls the map; `static` keeps the map still and moves your marker until it nears the rim. |
| `max_icons` | `64` | Most map icons drawn at once (8-128). |
| `label_font_size` | `17` | Height in pixels of the player and ping name labels (8-40). |
| `fog_strength` | `100` | Opacity of the grey over undiscovered areas (0-100). `0` turns the fog off. |
| `map_texture_size` | `8192` | `8192`, `4096`, `2048` or `1024`; see Map Renderer. |
| `map_renderer` | `gpu` | `gpu` or `cpu`; see Map Renderer. |
| `debug_logging` | `false` | Verbose diagnostics in `shroudtopia.log`. |

Keys are read every second while the mod is active, so most changes apply
without a restart.

Position and size are not config values: press `Esc`, drag the minimap where you
want it and drag its bottom-right corner to resize. The result is saved in
`mods\minimap_mod\minimap_layout.txt`.

Key names accepted by `toggle_key` / `heading_toggle_key`: `F1`-`F24`,
`insert`, `delete`, `home`, `end`, `pageup`, `pagedown`, `backspace`, and
`numpad-*`.

The minimap is always north-up: the map, frame and markers never rotate. The
player arrow points along the direction you travel, computed from the position
feed at no cost (standing still keeps the last direction; turning the camera
alone does not move it).

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
- `map_texture_size`: `8192` (default), `4096`, `2048` or `1024`. The HD map
  is box-filtered down to this size when it is loaded. `4096` cuts the map's
  VRAM from about 358 MB to 90 MB and is hard to tell apart at normal zoom.
  The CPU copy of the map is released as soon as the GPU upload has finished.

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

### Square window and frame

The minimap is a square window: the map shader, the CPU renderer, marker clipping
and the player arrow all use the square boundary. `embervale_minimap_frame.rgba`
is the thin frame with the N / E / S / W letters, generated with
`python tools\map-render\make_clean_frame.py`; the original ornate frame is kept
as `embervale_minimap_frame_ornate.rgba`.

### Fog of war

`embervale_minimap_fog.frag.spv` draws the undiscovered areas. The data is the
game's own `FogOfWar` grid (one byte per 8 world units, 1280x1280 for the
10240-unit world), copied from the running game twice a second and uploaded as
a texture only when it changed. The shader is generated by
`python tools\shaders\build_fog_shader.py`. Without the shader file the map is
drawn without fog.

### Icons, players and labels

Map icons are the game's own map marker icons at full resolution (64/128 px),
drawn on the GPU from a mipmapped sprite atlas (`embervale_minimap_sprite.frag.spv`)
in their original colors. Marker types that have no icon in the game's marker
registry are not drawn (the world map does not draw them either). Without the
sprite shader the icons fall back to a CPU rasterizer.

Other players come from the session's player list (position and name); pings
from the game's ping events; waypoints from each player's waypoint slot. Name
labels are rendered with GDI (Segoe UI, white with a dark outline) into the same
atlas. The atlas is rebuilt on a background thread whenever a new name shows
up, so a player joining does not stall the frame.

Waypoint outlines are silhouettes generated per icon at load time (the icon's
alpha grown by a few pixels), drawn under the icon in yellow.

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

## Performance Notes

- The mod adds one small `vkQueueSubmit` (a couple of quads and a few dozen
  sprites) per frame. Its command buffer is only recorded when the previous
  one for that swapchain image has finished; if the GPU is far behind, the
  overlay is skipped for that frame instead of stalling the game.
- Game-thread hooks read a few hundred bytes per frame; marker and player
  lists are mirrored at 20 Hz, the fog grid at 2 Hz.
- No memory scanning at runtime. The view direction is derived from the
  position feed, so there is no camera search.
- With the mod off (`F10`) nothing runs except the key check.

## Version

Current mod version: `0.4.46`.

The mod resolves hook signatures near the known Enshrouded client addresses at
load time, which makes small game updates less likely to break the minimap.
Large game updates can still require structure offsets to be revalidated.
