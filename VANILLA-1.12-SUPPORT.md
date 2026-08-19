# Noggit Cinder — WoW 1.12 (Vanilla / Turtle) support

Noggit Cinder is a fork of Noggit Red (a WotLK 3.3.5a editor), taught to **open a 1.12 client, read 1.12
(v256/257) M2 models, and read/edit/save 1.12 ADT terrain**. All vanilla behaviour is gated on
a new `ProjectVersion::VANILLA` so WotLK/SL projects are unchanged.

Verified empirically against a real 1.12 client (`G:\twmoa_1181`, Turtle WoW) using its own
StormLib + M2/DBC parsers: the M2 geometry chain (`sum(vcount)==nVertices`,
`sum(icount)==nTriangles` on a doodad, a character and a tree) and the DBC column layouts
(Map name field, AreaTable name field, 8-locale string width).

## Build

**Built and verified** (VS2022 Community + Qt 5.15.2 + this machine, 2026-08-18): compiles, links
to `noggit.exe`, and launches (creates a GL context and reaches the project-selection UI). The
submodules were a ZIP extract with no pinned commits, so two gotchas had to be resolved — both
unrelated to the vanilla changes:

1. **`blizzard-database-library` must be the `t1ti_fixes` branch**, not `main`. `main` is stale
   (2023-07-03) and predates `getBuild()` and the `BlizzardDatabaseRowDefiniton`→`Definition`
   typo fix, so the `src/noggit/database/` integration won't compile against it:
   ```bash
   cd src/external && rm -rf blizzard-database-library
   git clone --depth 1 -b t1ti_fixes https://gitlab.com/T1ti/blizzard-database-library.git
   ```
   `blizzard-archive-library` (default branch) is fine as-is.
2. **The source dir must be a git repo** — the `update_git_revision` pre-build step `FATAL_ERROR`s
   in a non-git tree. `git init && git commit --allow-empty -m baseline` is enough.

Everything else auto-resolves: StormLib/CascLib/Lua/Sol2/lodepng/Json/FastNoise2 come in via
FetchContent; only Qt5 is a manual dep (installed here via `aqtinstall` → `G:/Qt/5.15.2/msvc2019_64`).

Full recipe:
```bash
# deps (from repo root)
cd src/external
git clone https://gitlab.com/T1ti/blizzard-archive-library.git
git clone --depth 1 -b t1ti_fixes https://gitlab.com/T1ti/blizzard-database-library.git
cd ../.. && git clone --depth 1 -b dep-cmake https://gitlab.com/T1ti/build-dependencies.git cmake
git clone --depth 1 -b dist-definitions https://gitlab.com/prophecy-rp/build-dependencies.git dist/definitions
git clone --depth 1 -b dist-themes       https://gitlab.com/prophecy-rp/build-dependencies.git dist/themes
git init -q && git commit -q --allow-empty -m baseline     # for update_git_revision

# configure + build (Qt path adjust as needed)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=G:/Qt/5.15.2/msvc2019_64
cmake --build build --config RelWithDebInfo --target noggit --parallel
# -> build/bin/RelWithDebInfo/noggit.exe
# deploy Qt DLLs: windeployqt --release build/bin/RelWithDebInfo/noggit.exe   (Qt bin first on PATH)
```

The post-build MySQL-driver/themes copy step (`MSB3073`) may report failure *after* the exe is
already linked — it's a deploy convenience, not a compile error; the binary is complete.

Nothing here changes the build system; the vanilla work is source-only. `blizzard-archive-library`
is a static lib compiled into `noggit.exe`, so its two edited files rebuild with the main project.

## How to use

1. New Project → pick **Vanilla** in the version dropdown, point the client path at the 1.12
   client (e.g. `G:\twmoa_1181`), pick a project folder.
2. Open a map from the list, edit terrain/objects, save.

## What changed (by area)

### 1. Client version plumbing  (WotLK stays default)
- `project/ApplicationProject.h` — `ProjectVersion::VANILLA` already existed in the enum; now wired up.
- `project/ApplicationProject.cpp` — `loadProject` maps VANILLA → archive `ClientVersion::CLASSIC`,
  DB build `1.12.1.5875`, locale AUTO; `mapToEnumVersion`/`MapToStringVersion` handle "Vanilla"
  (and now return a value after `assert(false)` instead of UB).
- `project/ApplicationProjectReader.cpp` — reads "Vanilla" from `.noggitproj`.
- `ui/.../NoggitWindow.cpp` — **VANILLA joins WOTLK in the branch that calls `OpenDBs()`** (was the
  only place DBCs load; other versions `assert(false)`).
- `ui/.../NoggitProjectCreationDialog.{ui,cpp}`, `.../ProjectListItem.cpp` — "Vanilla" combo item,
  icon + list label (reuse the wrath icon; no vanilla icon resource).

### 2. Archive layer — 1.12 MPQ chain  (`blizzard-archive-library`)
- `include/ClientData.hpp` — new `ClientVersion::CLASSIC`; `VanillaArchiveNameTemplates`
  (`base/dbc/fonts/interface/misc/model/sound/speech/terrain/texture/wmo.MPQ` + `patch`,
  `patch-{number}`, `patch-{character}`) and `VanillaLocaleArchiveNameTemplates`.
- `src/ClientData.cpp` — storage type is now `SL ? CASC : MPQ` (so CLASSIC + WOTLK are MPQ);
  `initializeMPQStorage` branches to the vanilla templates for CLASSIC; **`validateLocale`
  tolerates locale-less clients** (Turtle keeps `realmlist.wtf` at the client root with no
  `Data/<locale>/` dir) by falling back to enUS instead of throwing.

### 3. M2 v256/257 reader  (the "read 1.12 models" headline)
- `ModelHeaders.h` — `ModelHeader_classic` (vanilla header: extra `playableAnimationLookup`
  array + embedded `nViews/ofsViews`, `nViews` at file offset `0x4C` — verified), `ModelViewV256`
  (embedded view, no SKIN magic, offsets relative to the .m2), `ModelGeosetV256` (**32-byte**
  submesh — verified: `d2` present, `vstart/vcount/istart/icount` at 4/6/8/10, single centre Vec3,
  no second box/radius).
- **CRITICAL vanilla-header gotcha — `texture_flipbooks`:** vanilla/BC headers (v256-263) carry an
  `M2Array texture_flipbooks` between `texture_weights` (transparency) and `texture_transforms`
  (texanims); WotLK removed it. Omitting those 8 bytes shifts EVERY later field — materials
  (renderflags), bone/texture/texunit/transparency/texanim lookup tables, bounding + collision
  boxes — so every vanilla model got garbage blend modes, wrong texture assignments (tex lookup
  read bone-lookup data → tree canopies bound the trunk texture) and junk bounds, while geometry
  (all fields BEFORE the gap) parsed fine. Verified empirically: all **9,689** v256/257 M2s in the
  Turtle client validate with the field present, only 39% without it (`m2_layout_scan.py`; e.g.
  `ElwynnTreeCanopy01.m2` really has 2 materials — trunk Opaque, canopy **Alpha_Key** — matching
  its 2 passes, and texture_lookup `(0,1)`). No blend-mode promotion hacks are needed: vanilla
  foliage is Alpha_Key *in the file* once the header is mapped right.
- `Model.{h,cpp}` — version gate accepts 256/257 for VANILLA (was a hard reject); classic header
  is re-read and normalised into the WotLK-shaped `header`; the view is parsed from **inside the
  .m2** (no external `.skin`) with geosets converted to `ModelGeoset`; animation + colour/alpha
  parsing is skipped (wider vanilla `AnimationBlock`), so models render statically in bind pose —
  geometry and textures are unaffected.

### 4. DBC layer  (verified vs the real client)
- `DBCFile.{h,cpp}` — `static int LocaleCount` (8 for vanilla, 16 for TBC+) drives
  `getLocalizedString`/`writeLocalizedString`; fixes the 17-vs-9-wide localized-string block.
- `DBC.cpp` — `OpenDBs` sets `LocaleCount`, opens each DBC independently (one bad file no longer
  skips the rest) and **skips the four tables absent pre-TBC**: `LiquidType`, `LightParams`,
  `LightSkybox`, `WMOAreaTable` (accessors already degrade on an empty table). `getMapName` reads
  the vanilla name field **3** (verified: `Map.dbc` = id/Directory/InstanceType/Name…) with an
  InternalName fallback. `AreaTable` name field 11 was already correct (verified).

### 5. ADT save → valid 1.12 output
Vanilla saving reuses the existing "mclq export" path, now driven by version, not a manual setting:
- `MapTile.cpp` — `saveTile` forces mclq mode for VANILLA (MCLQ liquid, no MH2O); MFBO is not
  emitted in mclq mode.
- `MapChunk.cpp` — MCCV chunk skipped and its header `has_mccv` flag cleared in mclq mode.
- `map_index.cpp` — WDT `MPHD` no longer sets `FLAG_SHADING` (0x2 = MCCV) for VANILLA.
- (Loading vanilla MCLQ terrain + optional MCCV/MH2O already worked in Noggit Red.)

WMOs need no change: vanilla and WotLK are both root version 17 and share the chunk set Noggit reads.

### 6. Runtime crash fixes (vanilla load/render path — found by actually running it)
The vanilla path had never been exercised, so several latent crashes surfaced when opening a
project and a map. All verified fixed by running against the Turtle client (opens project → map
list → tile grid → 3D editor with terrain + v256 models rendering, 1969 objects loaded):
- **Map list** (`BuildMapListComponent.cpp`): `std::stoi("")` on `ExpansionID` (absent in vanilla
  Map.dbc) crashed on project open — guard every column int-parse; fall back to the Directory name.
- **WDBC reader** (`blizzard-database-library` `WDBCTableReader.cpp`): `locstring` columns are read
  as a fixed 16 locales, but vanilla DBCs have 8, so the read over-runs the record — `_stringTable.at()`
  threw. Made the string lookups non-throwing (`safeStringAt`).
- **BitReader** (`BitReader.cpp`): the same over-read made `ReadUint32/64` throw `out_of_range` off
  the record end — return 0 instead (in-bounds columns still read correctly).
- **Missing DBCs** (`Utils.hpp` `readFileAsIMemStream`): loading a WotLK-only table (e.g.
  `MapDifficulty`, absent in vanilla) threw in `ClientFile`'s ctor when a map was selected — return a
  minimal empty WDBC so the table loads empty.
- **Model render** (`ModelRender.cpp` `prepareDraw`): vanilla models are drawn statically and skip the
  colour/transparency/texture-animation blocks, leaving those vectors empty while lookup tables still
  index them → access violations. Size-guard `_colors`, `_transparency`, `_texture_animations`.
- **Liquid render** (`LiquidRender.cpp`): `tex_frames.at(liquidID)` threw because vanilla has no
  LiquidType.dbc to populate the liquid texture-frame table — skip a layer whose type isn't present.
  Later superseded: `OpenDBs` now **synthesizes a LiquidType table for vanilla** (Water/Ocean/
  Magma/Slime + Green Lava, WotLK 45-field layout via `DBCFile::loadFromMemory`) pointing at the
  animated `XTextures\...` sets every 1.12 client ships — so MCLQ water renders and the water tool
  gets a working liquid list.
- **Liquid load** (`liquid_layer.cpp` `changeLiquidID`): with no LiquidType.dbc, `getByID()` threw
  `NotFound` for *every* water chunk — 22k+ throw/catch/log cycles when loading a full continent
  (Eastern Kingdoms), which brought the app down under the load. Check existence first and default
  to a river/water type instead of throwing per chunk.
- **Model texture bind** (`ModelRender.cpp` `bindTexture`): an edge-case model can carry a
  texture-combo/lookup index past the parsed tables, yielding a garbage texture reference that
  crashed in `upload()` — bounds-guard the index and skip that texture unit.
- **Material index clamp** (`Model.h` `render_flag_safe`): even with the corrected header, a
  malformed model over-indexing its materials now clamps to the last material instead of reading
  out of bounds (`ModelRender.cpp` routes every `_render_flags[...]` access through it).
- **Crash forensics** (`error_handling.cpp`): traceless crash classes (fail-fast `0xC0000409`
  stack-buffer overrun, heap corruption `0xC0000374`, stack overflow, `std::terminate`) bypass
  `SetUnhandledExceptionFilter`. Added: a vectored first-chance logger writing
  code/address/module to **`noggit_fatal.txt`** via plain stdio (the Log stream can be wedged by
  the crashing thread), a **minidump** (`noggit_crash_<pid>.dmp` next to the exe, open with the
  build's `.pdb` in VS/WinDbg) from both the unhandled filter and the fatal first-chance codes,
  and a `std::set_terminate` hook that names the uncaught exception. If `log.txt` ends silently,
  look at those two files. Two more traps removed: on Windows, `signal(SIGSEGV/SIGFPE/SIGILL)`
  is no longer registered (the CRT's `__except` around `main` claimed hardware faults BEFORE the
  unhandled filter, so crashes died reportless), and the signal handler ends in
  `TerminateProcess` instead of `exit()` (exit ran static destructors on a broken process with
  live threads — the teardown crashed again inside the texture cache and ate the report).
- **The Stormwind crash root cause** (`AsyncObjectMultimap.hpp`): `erase()` — which backs the
  texture, model AND wmo caches — dropped its mutex between "refcount hit zero" and
  "`_elements.erase`" (it must, because `ensure_deletable` blocks on the loader threads). In
  that window another thread could `emplace()` the same key: the count was 0 so it took the
  create path, the map emplace found the existing element and returned it *without
  constructing*, and the object was then destroyed by the eraser — leaving the second thread
  with a dangling object it had just queued for async load. The loader then wrote the object's
  file-path/members into freed, reallocated memory (captured in a minidump: a UTF-16 texture
  path scribbled over a live Qt tab-animation object → deterministic AV in
  `QObjectPrivate::isSignalConnected`). Texture churn while tiles stream in near Stormwind made
  the revive-vs-erase race hot. A second arm of the same family: with the refcount bouncing
  0 → 1 → 0 (release, revive, release), TWO erasers can be in flight for one key, and the slower
  one calls virtual methods on the object the faster one destroyed (captured: null-vtable
  virtual call, AV reading address 0x20, plus a bad_alloc from copying a garbage-length string —
  VM counters proved the process was NOT out of memory). Final protocol: `erase` decides under
  the lock; an `_erasing` TOMBSTONE makes the kill exclusive to one eraser; the owner re-checks
  the refcount after the unlocked wait and — if the key was revived — re-queues the object when
  the dequeue already pulled it from the loader (otherwise waiters hang); `ensure_deletable` is
  called UNCONDITIONALLY (`finished` becomes true inside finishLoading()/error_on_loading()
  while the loader thread still touches the object afterwards, so gating the wait on it was
  itself a use-after-free window); `emplace` only queues freshly *constructed* objects, so a
  revived object is never double-loaded.
- **Concurrent-logging hazard** (`Log.cpp`): `InitLogging` pointed cout/clog/cerr at ONE shared
  `std::filebuf` with no locking, while noggit logs concurrently from the async loader threads,
  the tile-update thread and the main thread (worst case: the ~600-line doodad-UID burst while
  the Stormwind tile loads). Racing writes on a shared filebuf are UB (heap-corruption class)
  and the unflushed buffer kept eating the tail of `log.txt` after crashes. Fixed with a
  synchronized line-assembling streambuf (thread_local line build-up, mutex + flush per
  completed line) — after this fix, crash stack traces finally reach `log.txt` intact (which is
  how the Qt-side stack of the UAF crash above was captured). Defensive hardening in the same pass:
  `initRenderBatches` clamps MOBA vertex ranges (raw file data) to the vertex count before
  filling `_render_batch_mapping`; `upload()`'s `flags==2 → unused[5]` material remap is now
  gated on `modern_features` (parity with the draw-call loop) + material index clamped;
  `WMOGroup::load` no longer dereferences `fogs[0]` on an empty MFOG.

Verified opening a full Blizzard continent (Eastern Kingdoms, ~5000 objects, very water-heavy) as
well as a clean custom zone (Sunnyglade Valley, all models rendering). Note: continents imported
from later game versions carry dead references to models/textures that a 1.12 client doesn't ship —
Noggit lists and skips them, and "Fix all UIDs" will refuse over them (use "Get max UID"); that's map
data, not a port issue.

### 7. The deep root cause of the streaming crashes (`blizzard-archive-library`)
`ClientData::readFile` trusted `SFileGetFileSize`, but StormLib returns **SFILE_INVALID_SIZE
(0xFFFFFFFF) on failure**. The failure path then: `buffer.resize(0xFFFFFFFF)` — a 4 GB
allocation whose zeroing stalls the app for seconds (the observed "freezes") or throws
bad_alloc — followed by `SFileReadFile(handle, buf, 0xFFFFFFFF)`, which walks the archive's
sector table with an invalid size and scribbles decompressed data at wild offsets → **heap
corruption** (captured repeatedly in minidumps as UTF-16 file paths / ntdll NT-path buffers
overwriting live Qt objects, and `-1`-poisoned pointers on multiple threads). The read-failure
`assert(false)` is compiled out in RelWithDebInfo, so callers received the garbage as a
"successfully loaded" file. The path fires constantly during tile streaming because Turtle map
data references WotLK-only files that don't exist in the 1.12 client (the every-session
`demolishercannonball.m2 could not be loaded` line is the canary) — densest around Stormwind,
hence "crashes when approaching Stormwind". Fixed: sizes of 0 / 0xFFFFFFFF / >512 MB are
rejected, failed reads are treated as not-found (no garbage buffers), file handles are closed
on every path, and the `(listfile)` load in `MPQArchive` got the same guard.
Related gotcha discovered on the way: the project compiles with **/EHa**, so `catch (...)` (e.g.
in `AsyncLoader::process`) also swallows ACCESS VIOLATIONS — a crashed parse can masquerade as
a mere "failed load" while leaving corrupted state behind. The first-chance-AV minidump in
`error_handling.cpp` exists to make such swallowed faults visible.

## Known limitations / follow-ups
- **Lighting/sky is partial**: `LightParams`/`LightSkybox` don't exist in 1.12 and `Sky.cpp`'s
  zone-light load is WOTLK-gated. Terrain/models render; ambient sky may be flat. Not a crash.
- **M2 animation not parsed** for v256 (static bind pose). Fine for doodad placement; no animated
  preview.
- **Map Creation Wizard** still writes WotLK-indexed `Map.dbc` columns — don't use it to author
  vanilla `Map.dbc` rows without adjusting those indices.
- **Big-alpha → 4-bit**: a map imported from a WotLK WDT keeps `mBigAlpha`; run Noggit's alpha
  conversion before saving for a strict 1.12 client. Native/new vanilla maps are already 4-bit.
- `ModelGeosetV256`'s tail (d5/d6/centre) beyond `vstart/vcount/istart/icount` is best-effort;
  only the verified fields feed rendering.
