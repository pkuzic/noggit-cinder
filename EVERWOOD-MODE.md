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
| Sculpt: Smooth | Soft (falloff) or hard (uniform, at most half strength). Always a sphere. **Also remove floaters under the brush** (default on): when the stroke ends, the floaters under it are removed in the same undo step (see Remove floaters). |
| Sculpt: Push out, Pull in | Grows sideways out of a surface or carves into it, along the surface normal or the view direction. Optional softened edge. |
| Flatten | Levels toward the height clicked at the start of the stroke, plus an offset. |
| Bridge / Tunnel | Click point A, point B, then click again or press **Build**. One stroke of hard edits along the path, with an arch and a deck thickness; up to 160 yd and 400 edits, as in the game. |
| Ramp / Path | Click the ground to lay out a path (two or more points); click the last point again or press **Build ramp** (Cancel clears it). Options: Width (flat width, yd), Slope (**Follow the clicked heights**: straight between the points' heights; **Fixed angle**: leaves the first point at the given angle, + rises / - falls), Edge softness (Sharp 60 deg / Normal 45 deg / Soft 30 deg bank), Strength, **Smooth edges**. One stroke = one undo step. See Ramp / Path below. |
| Remove floaters (button, every tool but Water flow) | Digs away tiny islands of ground that hang in the air, anywhere in the edited area; **Max floater size (voxels)** (default 64, 0.5 yd voxels). See Remove floaters below. |
| Paint (caves) | CONTRACT v3.10 3D voxel paint (journal mode 7): paints dug or built faces (cave floors, walls, ceilings) with a texture of the shared palette, through the air the brush reaches. See below. |

Hold the left mouse button to paint a stroke. Alt + wheel changes the radius. One stroke is one undo: Ctrl+Z,
Noggit's action history and the panel's buttons all work on it. Undoing rebuilds the affected chunks from the
remaining edits, the same canonical rebuild as the game.

The seed reads the raw ADT through Noggit's project-then-client file lookup, the same bytes the game reads. A tile
whose heights or holes were changed in this session with Noggit's other tools (raise, lower, smooth, holes, their
undo) seeds from memory instead, built with exactly the floats `MapChunk::save` writes, so the voxel surface follows
the terrain at once: about 0.3 s after the terrain stops changing, the layer replays every edit on the new heights
(the log says "reseeding from the live heights"). After a save the file gives the same seed bit for bit. Before
this, a raised ground stayed hidden under the edit while the voxel surface kept the old ground, and the seam opened
into a hole. **Re-seed from saved ADTs** replays every edit on the heights as saved on disk now (use it after
replacing ADT files outside Noggit).

## Remove floaters
Tiny pieces of ground can end up floating (a thin ledge dug away from below, a build blob in the air); no brush
stroke removes them without biting the ground next to them. **Remove floaters** (panel button, whole edited
area = every active 16 yd block, one undo step) and the Smooth checkbox find them and dig them away with ORDINARY
hard dig edits (mode 0, sphere, strength 255) that the game replays like any other; no replay maths and no wire
format changed. The status line says `Removed 7 floaters (31 edits)`, and `, left N` for floaters it found no safe dig for.
- A **floater** is a 6-connected set of solid samples (density < 0) of at most *Max floater size* samples that is not
  grounded. Grounded = some sample has a solid 6-neighbour outside the active blocks (the seeded ground continues
  there: below the active column and beside the edited area), or the component grows past the size limit. So
  terrain, overhangs, arches and bridges, which all reach the seeded ground or a larger body through solid samples,
  are never floaters. A piece that touches the ground only diagonally (a corner contact) is a floater too: it is the
  typical stubborn voxel stuck on a slope, and it is removed like the others.
- **Dig safety**: before any dig is planned it is dry-run on a scratch copy of the field with the real edit
  function (`ApplyEditSet`, inactive blocks seeded): it is accepted only if every sample it changes is a floater
  sample, or air (density >= 0) with no solid non-floater sample in its 26-neighbourhood, and every floater sample
  ends up air. The smallest ball covering the floater is tried first; if it would bite the ground the floater is
  split along its longest axis and each half is covered separately, down to one dig per sample (smallest valid
  radius). A floater with no safe sequence is skipped and counted in `left`. Floaters whose digs only touch each
  other (neighbouring floaters) are planned together.
- The Smooth checkbox runs the same pass at the end of a Smooth stroke over the floaters having a sample within one
  voxel of one of the stroke's dabs; its digs carry the stroke's id, so Undo takes the stroke and the digs together.
- Code: `src/noggit/everwood/VoxelFloaters.{hpp,cpp}` (pure C++), `EverwoodLayer::planFloaters`,
  `EverwoodTool::removeFloaters`.

## Ramp / Path
A ground path set with the mouse, levelled to the wanted grade. The tool emits ordinary edits the game already
replays: mode 6 (LEVEL, with a soft bank) along the path, optionally followed by mode 2 (smooth, 1 pass, 50 %).
- LEVEL clamps the field into a window around a horizontal plane (`param` = plane offset in cm from `pos.z`, bits
  12-13 the bank slope, bits 14-15 the core size): flat inside the dab's core, a bank of the chosen slope out to its
  radius, so it cuts high ground and fills low ground. Dabs laid along the path, each with the plane height of its
  place, make the slope. A later dab overwrites an earlier one inside its core, so a dab's plane is the path height
  one core radius behind it and the dabs run one core radius past the end: the surface is exactly the requested
  line from the first to the last point (a short flat landing follows the last point). Pass 1: two bank tracks
  just outside the flat strip (radius 2 x the bank width: 2 / 4 / 8 yd for Sharp / Normal / Soft); pass 2: the flat
  strip with small dabs (radius Width / 2 within 2..6 yd) on an odd number of tracks across it, the centre line
  included; dabs are spaced 0.6-0.8 core radii. Where the ground is farther from the plane than a dab's vertical box
  (pos.z +- radius), the dab is stacked (same plane and xy, one per vertical slab) so a deep cut or fill is reached.
- Limits: path 2..300 yd, at most 700 edits, width 1..32 yd, every radius <= 16 yd. A 20 yd ramp of width 6 is about
  70-100 edits (a deep cut with Sharp banks needs more).
- Measured with the smoke test (`expect_ramp_line`): surface within 0.17 yd of the requested line on a 14 degree
  ramp in a hillside, 0.12 yd on a 12 degree fixed-angle ramp, 0.07 yd on an 8 degree falling polyline.
- Code: `EverwoodTool::rampEdits`, `drawRamp` (the live preview: centre line, flat edges, bank edges).

## Painting voxel ground

Use the normal **Texture Paint** tool (Shift + left mouse). The cursor and the brush land on the voxel surface where
the ray meets it around an edit: on top of a build, on the floor of a dig. Painting edits the ADT chunk's own layers
and alpha maps, as everywhere else, and the mesh is rebuilt from them at once, so no Re-seed or reopen is needed.
Vertex paint and shadow edits are picked up the same way. Undo works through Noggit's action history as usual, and
the paint is saved with the ADT. The voxel edits, their wire format and their density do not change.

Steep walls and undersides keep the fixed wall look of CONTRACT v3.3.6, as the game does: painting changes the ADT
layers under them, which shows only on the tops and gentle slopes.

### Paint (caves): the 3D voxel paint (CONTRACT v3.10, mode 7)

Texture Paint is 2D: a cave floor would paint the ground above it. **Tool > Paint (caves)** paints the voxel
volume instead. Each dab is one mode-7 edit at the hit + 0.5 yd along the surface normal (the air side); it floods
through the air inside its sphere (never through a ceiling or a wall) and paints that air plus the solid samples
bordering it. Options: a texture (only the 729 textures of the shared palette, `PaintPalette.hpp`: pick it in the
list, type part of its name, drop one onto the preview, or take the Texture Paint tool's texture), radius 0.5-8 yd
(default 3), opacity (default 30 %), falloff, **Erase this texture**, **Reveal** (fades every cave texture).
A dab is refused, with a hint under the panel, on untouched ground (edited weight < 0.5: use Texture Paint
there) and when a 16-yd block it touches already holds 4 other cave textures. A stroke of refused dabs leaves no
undo step. Undo and redo rebuild exactly the chunks whose paint can change.

Drawing: a painted chunk's mesh carries 4 slot weights per vertex (trilinear of the 8 lattice samples around it)
and its 4 slot textures (its own chunk's materials, then those of the neighbours its window apron reads). Each slot
is mapped like the face under it (the ground's triplanar on the top part, the wall's sharp triplanar on the wall
part) and blended over the ground and the wall; a painted wall loses the edited-wall light lift in proportion. The
palette textures use the samplers the ground layers left free (11 in all); a slot that does not fit is left out
and logged once. Unpainted meshes are drawn exactly as before. Not exported to the ADT.

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
"wire":"EDIT3 CONTRACT v3.10",
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
  CONTRACT v3.10 adds mode 7 (3D paint, radius <= 8): `param` bits 0-10 palette id, 11-12 falloff, 13 erase,
  14 reveal. Files written before v3.10 (`"wire":"EDIT3 CONTRACT v3.6"`) load unchanged; the version stays 1.

### `.mvoxexport` format, version 1

The same `edits` array, plus what a server import needs to decide whether it may replay them:

```json
{
"format":"mvoxexport",
"version":1,
"wire":"EDIT3 CONTRACT v3.10",
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
(`golden-v3.9.txt` — CONTRACT v3.9 dithered soft rounding + mode 6 level with a soft bank — and the older
`golden-v3.6.txt`, from `benilla-voxel/tests/golden_v3.rs`).

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
Paint (caves): `set tool paint`, `set texture <id|path|part>`, `set opacity <pct>`, `set paint_falloff
smooth|linear|flat`, `set erase 0|1`, `set reveal 0|1`; `paint_ray ox oy oz dx dy dz [ok|refused]`, `anchor` (the
last edit's position), `paint_rel ...` (paint_ray from the anchor), `camera_anchor dist height [side]`,
`camera_look cx cy cz tx ty tz` (both relative to the anchor), `expect_painted [>]n`, `phash_save <name>`,
`phash_same <name>`, `phash_differs <name>`; `status` also prints the painted chunks and the paint hash.
Remove floaters / Ramp: `set floaters 0|1` (Smooth checkbox), `set floater_max <n>`, `remove_floaters [max]`,
`expect_floaters <n|>n> [max]`, `list_floaters [max]`, `floater_snap <name> [max]` + `expect_floater_removal <name>`
(floater samples gone, every other solid sample byte-identical, nothing lowered), `expect_solid x y z [0|1]`;
`set tool ramp`, `set ramp_width|ramp_slope follow|angle|ramp_angle|ramp_edge sharp|normal|soft|ramp_smooth`,
`ramp_point x y [z]` (WoW axes; z = the surface when omitted), `ramp_apply [ok|refused]`, `ramp_cancel`,
`expect_ramp_line follow|<deg> <tol> [skip]` (the surface along the last ramp's centre line), `surface_line x0 y0 z0
x1 y1 z1 tol [skip]`.
`seed_journal <file> [MapDirectory]` (before `map`; default `Azeroth`) makes the project's `.mvoxj` a copy of
`<file>`, so a script can start from the edits a previous session left behind.

Only the script drives the window: real mouse, wheel, key, tablet and touch input from the desktop is ignored
during a smoke run (a click on that screen would otherwise start a stroke between the scripted ones and shift every
undo after it). When any arrived, the result file says so before `RESULT` (`note: ignored real desktop input`).

Undo, a stroke at a time: every stroke that changes something is one step of Noggit's action history. A new stroke
never reuses the id of an edit still in the journal (loaded or undone). The undone strokes stop being redoable when
the next stroke opens its undo step, together with Noggit's own redo steps; a stroke that leaves no step (every
paint dab refused) leaves redo alone.

## WMO scale

The last u16 of a MODF entry (+0x3E) is the WMO's uniform scale, 1024 = 1.0. Stock tiles store 0, which also
means 1.0. The Everwood game client and the server's vmap extractor honour it.

- **Every project, Everwood or not:** a stored scale is read, drawn, picked and written back, so opening and saving
  a tile never changes a scaled WMO. An unscaled WMO is written as 1024, exactly as before. The MODF extents are
  the box of the scaled model.
- **Everwood Mode only:** the scale can be edited with the object editor's scale field, the mouse and keyboard
  scale shortcuts, the gizmo (Scale), multi-select, the node editor and scripts. The range is 0.1 to 20, stored
  as round(scale x 1024), uniform only. Undo and redo cover it. Outside Everwood Mode these controls leave a
  WMO's scale alone. Random size on paste never scales WMOs.

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

Воксельный слой следует за ландшафтом: если поднять, опустить или сгладить землю обычными инструментами Noggit
под воксельной правкой или рядом с ней, примерно через 0,3 с после последнего мазка все правки пересчитываются
на новых высотах, и шов между землёй и вокселями не раскрывается в дыру. После сохранения тайла высоты из файла
совпадают с этими до бита. **Re-seed from saved ADTs** нужен только после замены ADT-файлов вне Noggit.

## Масштаб WMO

Последний u16 записи MODF (+0x3E) задаёт равномерный масштаб WMO, 1024 = 1.0; в обычных тайлах там 0, это тоже 1.0.
Клиент Everwood и vmap-экстрактор сервера его учитывают. В любом проекте сохранённый масштаб читается, рисуется и
записывается обратно, так что открытие и сохранение тайла не меняет масштабированный WMO; WMO без масштаба
пишется как 1024, как и раньше; границы MODF считаются по масштабированной модели. Менять масштаб можно только
в Everwood Mode: поле масштаба, мышь и клавиши, гизмо, мультивыделение, редактор нод и скрипты; диапазон от 0.1 до 20,
хранится round(масштаб x 1024), только равномерный; undo и redo работают. Вне Everwood Mode эти элементы масштаб WMO
не трогают.
