# Everwood water flow — file contract (v1, 2026-11-08)

Authored river/sea current for the Everwood client (benilla). Stored in the vanilla ADT's own
MCLQ liquid layer, in the `SWFlowv` slots every 1.12 MCLQ already carries and no stock tile uses
(`n_flowvs == 0` on all Blizzard tiles; the slots hold FLT_MAX junk). No new chunk, no new file:
a tile with flow still loads in any 1.12 reader.

## Bytes (per MCLQ layer, after the 8x8 tile bytes)

```
uint32 n_flowvs;          // 0, 1 or 2 — the ONLY validity flag readers trust
SWFlowv flowvs[2];        // always 2 slots on disk, 40 bytes each
struct SWFlowv {          // all little-endian float32
  float pos[3];           // centre, WoW WORLD coordinates (.gps): x = north, y = west, z = up
  float radius;           // influence radius, yards (> 0)
  float dir[3];           // unit direction in WoW world coords, horizontal (dir[2] = 0)
  float velocity;         // surface drift speed, yards per second, 0..8
  float amplitude;        // turbulence 0..1 (extra chop/foam in the current)
  float frequency;        // reserved: write 0, preserve on load
};
```

Writers: unused slots (index >= n_flowvs) are written as zeros. Readers: ignore slots >= n_flowvs
and any slot with a non-finite value, radius <= 0, |dir| < 0.01 or |pos| > 1e6.
Every MCLQ layer of one MCNK carries the same flow (the editor stores flow per chunk).

## Meaning (what the client does with it)

Each liquid vertex's flow vector = weighted average over the flow points of its own chunk AND its
8 neighbour chunks (so the field is continuous across chunk borders):

```
w_k  = max(0, 1 - dist_xy(vertex, pos_k) / radius_k)^2
flow = sum(w_k * dir_k * velocity_k) / sum(w_k)       (0 when sum(w_k) == 0)
turb = sum(w_k * amplitude_k) / sum(w_k)
```

The shader scrolls the water's normal/foam detail along `flow` (two-phase flow map, no stretching),
stretches sun glints along it and adds `turb`-scaled chop. Zero flow = the still-water look.

## Editor (Noggit Cinder, Everwood mode only)

Everwood tool panel → kind **Flow**: drag across water to paint the direction (each chunk under
the brush that has water gets one flow point at the brush centre, dir = drag direction, radius =
brush radius, velocity / turbulence from the panel); **Erase** clears a chunk's flow. Arrows are
drawn over the water of every chunk that has flow while the Flow kind is selected.
