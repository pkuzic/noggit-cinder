# Everwood Mode

A per-project setting that adds the voxel tools of the game's EME map editor to Noggit's map view. Voxel
edits made here use the same maths as the game client, so an edit lands on exactly the same samples.
ADT files are never changed by voxel edits: the voxel layer is an overlay stored next to the project.

With the setting off (the default, and for every existing project) none of this code runs: no panel, no
voxel layer, the terrain shader is compiled without the carve mask, and tiles save byte for byte as before.

## Turning it on

- New project: the **Everwood Mode** checkbox in the project creation dialog.
- Existing project: **Noggit > Project Settings... > Everwood Mode**. It takes effect the next time a map is
  opened.

It is stored as `"EverwoodMode": true` in the `Project` object of the `.noggitproj` file. An absent key means
false, and the key is only written when true, so a project that never used it does not change.

## The tools

The **Everwood** entry at the end of the left toolbar (the terrain icon, tooltip "Everwood") opens the panel:

| Tool | What it does |
|---|---|
| Sculpt: Lower / Dig, Raise / Build | Soft by default: moves the ground vertically with a falloff (smooth, linear, flat) and a hardness. **Hard brush** switches to the CSG dig and build. Sphere, cube, upright cylinder. |
| Sculpt: Smooth | Soft (falloff) or hard (uniform, at most half strength). Always a sphere. |
| Sculpt: Push out, Pull in | Grows sideways out of a surface or carves into it, along the surface normal or the view direction. Optional softened edge. |
| Flatten | Levels toward the height clicked at the start of the stroke, plus an offset. |
| Bridge / Tunnel | Click point A, point B, then click again or press **Build**. One stroke of hard edits along the path, with an arch and a deck thickness; up to 160 yd and 400 edits, as in the game. |

Hold the left mouse button to paint a stroke. Alt + wheel changes the radius. One stroke is one undo: Ctrl+Z,
Noggit's action history and the panel's buttons all work on it. Undoing rebuilds the affected chunks from the
remaining edits, the same canonical rebuild as the game.

**Re-seed from saved ADTs** replays every edit on the heights as saved on disk now. The seed reads the raw ADT
through Noggit's project-then-client file lookup, so unsaved terrain edits do not move the voxel surface until
they are saved and re-seeded. Terrain sculpted under an edited area with Noggit's other tools still edits the ADT
as usual, but that part of the ADT stays hidden while the voxel edit is there.

## Painting voxel ground

Use the normal **Texture Paint** tool (Shift + left mouse). The cursor and the brush land on the voxel surface where
the ray meets it around an edit: on top of a build, on the floor of a dig. Painting edits the ADT chunk's own layers
and alpha maps, as everywhere else, and the mesh is rebuilt from them at once, so no Re-seed or reopen is needed.
Vertex paint and shadow edits are picked up the same way. Undo works through Noggit's action history as usual, and
the paint is saved with the ADT. The voxel edits, their wire format and their density do not change.

Steep walls and undersides keep the fixed wall look of CONTRACT v3.3.6, as the game does: painting changes the ADT
layers under them, which shows only on the tops and gentle slopes.

## How it is drawn

The voxel mesh is drawn from the density with the game's rules for what counts as edited (`edited_weights`,
`seed_tilt_cos`, `RENDER_DROP`). One deliberate difference from the game: the game hides whole 16-yd columns of
ADT because its collision and shaders work per column. An editor has no such need, so here the ADT is hidden
only over the 0.5-yd lattice cells whose columns actually changed, and the mesh is drawn over those cells plus
a two-cell overlap. Untouched ground stays the real ADT with all its layers, shadows and vertex colours. The
density, the edits and everything saved are unaffected by this.

The mesh takes everything from the live ADT chunk under it: layers, alpha maps (sampled as the terrain's GL_LINEAR
does), specular flags, MCCV vertex colours and MCSH shadows, with the terrain's lighting, in the terrain's normal
convention (WoW x, up, WoW y). A triangle across an ADT chunk line is cut along it, so the seam lies where the
terrain's lies. Its shader also draws the brush cursor, the grid lines, the height contours and the normals view.
It does not have animated layers, Mists height blending, wireframe or the per-chunk overlays (area, impassable,
paintability, selection, ground effects).

## Files

Everything goes under `<project>/everwood/`:

- `voxels/<MapDirectory>.mvoxj`: the map's voxel edits. Written after every stroke, undo and redo, and read when
  the map is opened.
- `export/<MapDirectory>-voxel-edits-<timestamp>.mvoxexport`: written by **Export voxel edits**.

### `.mvoxj` format, version 1

JSON, one edit per line:

```json
{
"format":"mvoxj",
"version":1,
"wire":"EDIT3 CONTRACT v3.6",
"mapDirectory":"Azeroth",
"map":0,
"nextSeq":3,
"nextNonce":3,
"nextStroke":3,
"edits":[
{"seq":1,"wire":"02010001000000...","stroke":1,"nonce":1,"map":0,"x":-8900,"y":-90,"z":81.5,"radius":8,"mode":0,"strength":255,"param":0},
{"seq":2,"wire":"02020002000000...","stroke":2,"nonce":2,"map":0,"x":-8900,"y":-90,"z":81.5,"radius":4,"mode":2,"strength":51,"param":32768}
]
}
```

- `wire` is authoritative: the 54 hex characters of the 27-byte EDIT3 record of CONTRACT section 9.1 (`op` 0x02,
  `nonce`, `stroke`, `map`, `x`, `y`, `z`, `radius`, `mode`, `strength`, `param`, little-endian). It holds the
  floats bit for bit. The named fields repeat it for people and are ignored on load.
- `seq` is the local order the edits were made in, ascending. The field is seed + every edit in `seq` order.
- Coordinates are WoW axes (x north, y west, z up) in yards. `stroke` and `nonce` follow the contract: strokes
  are 1..65535, wrapping to 1.
- Only live edits are stored; undone strokes are dropped from the file.
- `mode`: 0 dig or lower, 1 build or raise, 2 smooth, 3 flatten. `param`: bit 15 soft (bits 0-1 falloff, 2-8
  hardness), bits 9-10 shape (1 cube, 2 cylinder) on modes 0 and 1; a signed plane offset in cm on mode 3.

### `.mvoxexport` format, version 1

The same `edits` array, plus what a server import needs to decide whether it may replay them:

```json
{
"format":"mvoxexport",
"version":1,
"wire":"EDIT3 CONTRACT v3.6",
"exportedAt":"2026-11-05T12:00:00",
"noggitVersion":"...",
"project":"MyProject",
"mapDirectory":"Azeroth",
"map":0,
"bounds":{"min":[-8908.0,-98.0,73.5],"max":[-8892.0,-82.0,89.5]},
"seedTiles":[{"x":32,"y":48,"size":1854681,"md5":"..."}],
"strokes":[{"stroke":1,"edits":1,"firstSeq":1,"lastSeq":1}],
"edits":[ ... ]
}
```

- `seedTiles` names every ADT under the edits with the size and MD5 of the bytes they were seeded from. Voxel
  edits are only meaningful on the same ground: an import should warn or refuse when the server's or client's
  ADT differs.
- Replaying means sending each `wire` in `seq` order as `.mvox <hex>` (rate, range and project rules of the
  contract apply). Exporting changes nothing and can be repeated.

## Tests

`src/noggit/everwood/VoxelGolden.cpp` checks the field against the game client's golden vectors
(`golden-v3.6.txt`, from `benilla-voxel/tests/golden_v3.rs`).

Setting the environment variable `NOGGIT_EVERWOOD_SMOKE=<script file>` makes Noggit run a test script instead of
showing the project selection window. Unset, the hook does nothing. While it is set, every default `QSettings`
goes to `settings/` next to the script, and results go to `<script>_result.txt`. Commands, one per line:

`window x y w h`, `project <dir>`, `map <id>`, `enter x y z yaw pitch`, `camera x y z yaw pitch` (WoW axes),
`wait <ms>`, `waittile <x> <z>`, `shot <png>`, `panel_shot <png>`, `save_tile <x> <z>`, `report <text>`, `exit`,
`golden <file>`, `project_everwood 0|1`, `expect_project_everwood 0|1`, `expect_no_everwood`, `tool`,
`set <option> <value>`, `move x y`, `press x y`, `release x y`, `undo`, `redo`,
`edit mode x y z radius strength param`, `bridge ax ay az bx by bz`, `ground x y`, `status`,
`expect_edits [>]n`, `expect_hidden [>]n`, `hash_save <name>`, `hash_same <name>`, `hash_differs <name>`,
`export`, `reseed`, `objects 0|1`, `camera_last dist height [side]`, `return_to_menu`, `settings_dialog_shot <png>`,
`creation_dialog_shot <png>`, `paint_tool <texture> <radius> <pressure> <hardness>`, `everwood_tool`, `shift 0|1`,
`cursor [0|1]`, `overlay shadows|mccv|lines 0|1`, `tint x y radius r g b`, `action_undo`, `action_redo`.

## Credits

The voxel field, the terrain seed and the brushes are ported from the Everwood game client's open crates
`benilla-voxel`, `benilla-formats`, `benilla-adt`, `benilla-world` and `eme` (Apache-2.0 / MIT), through the
server replica `MonkeyVoxelField`. The surface extraction is a port of `fast-surface-nets` 0.2.1 by Duncan
Fairbanks (Apache-2.0 / MIT).

---

# Everwood Mode (по-русски)

Настройка проекта: добавляет в окно карты воксельные инструменты игрового редактора EME. Математика та же, что
в клиенте, поэтому правка ложится на те же самые точки сетки. ADT-файлы воксельные правки не меняют: слой хранится
рядом с проектом, в `<проект>/everwood/voxels/<MapDirectory>.mvoxj`. Выключено по умолчанию; при выключенной настройке
этот код не работает вовсе, тайлы сохраняются байт в байт как раньше.

Включается галочкой **Everwood Mode** при создании проекта или в **Noggit > Project Settings...**; вступает в силу при
следующем открытии карты. Инструменты: лепка (копать, насыпать, сглаживать, вдавить, выдавить), выравнивание, мост и
тоннель. Кнопка **Export voxel edits** пишет файл для будущего импорта на сервер; форматы описаны выше.
