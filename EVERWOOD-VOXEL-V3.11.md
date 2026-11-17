# Everwood voxel CONTRACT v3.11 (proposal, 2026-11-17)

Why: mapper feedback on the Noggit voxel tools: Smooth is far too slow, the cave paint brush stops at
8 yd and has no hardness. Both need the game (benilla-voxel) to replay the new edits exactly like
Noggit, so they are one contract bump. Everything here is backward compatible: every edit that is
valid in v3.10 replays bit-identically.

Wire record (EDIT3, 27 bytes) is unchanged. Only meanings of currently unused param bits change.

## 1. Smooth passes (mode 2)

Today one smooth edit = ONE 6-neighbour Jacobi step over the pre-edit gather, rounded to i8. A step
moves the field by about one voxel, and the i8 rounding stalls small changes, so a mapper needs
dozens of strokes on a big slope.

v3.11: on mode 2 (hard AND soft), param bits 9-10 (free today: smooth is always a sphere, and the
shape bits only apply to modes 0/1) = pass count:

| bits 9-10 | passes |
|---|---|
| 0 | 1 (exactly v3.10) |
| 1 | 2 |
| 2 | 4 |
| 3 | 8 |

Algorithm for N passes:
1. Gather the pre-edit grid over the brush box grown by N voxels (v3.10 grows by 1), samples
   outside the active set read `outside(x,y,z)` as today.
2. Convert the gather to float once.
3. Repeat N times: Jacobi step into a second float buffer, applied ONLY to samples inside the brush
   (the same sphere / soft-weight test as today, same `a`); samples outside the brush keep their
   gathered value; swap buffers. The per-sample formula is unchanged:
   hard `old*(1-s) + avg*s`, soft `old + (avg-old)*a`, where `avg` is the mean of the 6 neighbours
   in the CURRENT float buffer.
4. Round ONCE at the end with the existing rule (`quantRaw`, or `DitherRound` for v3.9 dithered
   soft edits), write samples whose i8 value changed.
For N = 1 this is exactly the v3.10 result (one step, one rounding).

Rebuild closure: UNCHANGED (1 ring). Only brush samples are written and each reads its 6 face
neighbours; non-brush samples keep their gathered value, so by induction the result depends only on
brush ∪ 1-ring for any N (agreed with the game side 2026-11-17). The gather may be grown by N; the
extra samples are never read.

Implemented in the game: benilla 858266a5d (everwood-perf-water-1116, merged into
everwood-release-1117). Golden: `G:\mangos\disposable\voxel-core\golden-v3.11.txt` (v3.10 seq 1-147
read with v3.11 meanings + seq 148-162; whole set density f310ab9ee3d481c2, paint3
52700d3ab7a49009). Old golden edits 100 (param 0x0200 = 2-pass smooth), 145 (mode 7 r 8.5 now
valid) and 146 (bit 15 = HARD core 25 %) change meaning, so golden-v3.6/3.9/3.10 replay only
through a compat shim.

Valid range unchanged (radius <= 16 yd). Cost at r = 16 yd, N = 8 is ~2.2 M sample-steps: fine.

## 2. Cave paint (mode 7): bigger brush + hardness

a) `PAINT3_MAX_RADIUS` 8 -> 16 yd. `Edit::IsValid` allows mode 7 up to 16 yd. Nothing else
   changes (the 4-textures-per-block refusal is evaluated over every touched block, as today).

b) Hardness: param bit 15 is "ignored" today. v3.11: bit 15 = HARD flag. When set, bits 11-12
   select a hardness core instead of a falloff:

| bit 15 | bits 11-12 | weight w(d) with u = d / r |
|---|---|---|
| 0 | 0 / 1 / 2 / 3 | v3.10: smooth / linear / flat / smooth |
| 1 | 0 | core 25%: w = 1 for u <= 0.25, else smooth falloff over t = (u-0.25)/0.75 |
| 1 | 1 | core 50% |
| 1 | 2 | core 75% |
| 1 | 3 | core 90% |

Exact op order (f32, as the game implements it): `u = d/r; w = 1 if u <= c else { t = (u-c)/(1.0-c);
v = 1-t; w = (v*v)*(3-2v) }; A = round(s*w*255)` clamped 0..255; core index = raw bits 11-12, no remap.

CHECK FIRST on both sides: no writer has ever set bit 15 on a mode-7 edit (Noggit Paint3Param::
ToParam never does; confirm the game's eme paint tool). If one did, use a different flag.

## Out of scope (Noggit-only, no game change)

Remove floaters (Noggit emits ordinary dig edits), Ramp/Path tool (series of flatten edits), paint
spray + pressure (Noggit scatters / scales ordinary dabs), the 4 FPS and Shift+click fixes.

## Golden

Add golden cases to the shared golden file: smooth N = 2/4/8 hard + soft + soft-dithered across a
chunk border; mode 7 at r = 12 and 16; bit-15 cores 0..3. Noggit `VoxelGolden.cpp` and the Rust
tests must produce identical density / paint hashes.
