// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Everwood "remove floaters": find the small solid islands of the voxel field that hang in the air (a normal
// brush stroke cannot dig them away: a dab of any size that reaches them also bites the ground) and remove them
// with ORDINARY hard dig edits (mode 0, sphere, strength 255) the game already replays. Nothing here changes the
// replay maths or the wire format; a dig is only ever planned after a dry run on a scratch copy proved that it
// changes nothing but the floater (and the air right around it).
//
// Pure C++ (no Qt / OpenGL), on top of VoxelField.hpp. Lattice coordinates: sample i sits at wow i * 0.5; solid
// = density < 0 (SDF in 1/64 voxel units), the same sign the surface mesher uses.
//
// Floater = a 6-connected set of solid samples that
//   * has at most `maxSize` samples, and
//   * is not grounded: none of its samples has a 6-neighbour that is solid outside the active chunks (that
//     sample is the seeded ground, which continues below / beside the active region), and it never joins a
//     component larger than maxSize.
// Terrain, overhangs, arches and bridges are attached to the seeded ground (or to a component larger than
// maxSize) through solid samples, so they are never floaters. A floater that touches other solid ground only
// diagonally (26-neighbourhood, a corner contact the mesher joins into one surface: the tiny stubborn voxels stuck
// on the ground) is flagged `touching`; it is removed like any other floater, and the dry run still guarantees that
// no ground sample changes.
#pragma once

#include <noggit/everwood/VoxelField.hpp>

#include <cstdint>
#include <vector>

namespace Noggit::Everwood::Voxel
{
  struct FloaterRegion
  {
    bool all = true;                              // every active chunk
    Key lo{ 0, 0, 0 }, hi{ 0, 0, 0 };             // else: inclusive lattice box; a floater counts when any sample is in it
    // Optional, with a box: the floater must also have a sample within one voxel of one of these balls (lattice
    // units: centre x, y, z, radius), i.e. be under a brush, not only inside the brushes' bounding box.
    std::vector<std::array<float, 4>> balls;
  };

  struct Floater
  {
    std::vector<Key> samples;                     // lattice coordinates of its solid samples
    bool touching = false;                        // another solid sample touches it diagonally only
  };

  // Every floater of the region (`touching` ones included). maxSize is clamped to 1..16384.
  std::vector<Floater> FindFloaters(World& world, uint32_t maxSize, FloaterRegion const& region);

  struct FloaterPlan
  {
    std::vector<Edit> digs;                       // ordinary mode-0 hard digs, in application order
    uint32_t found = 0;                           // floaters in the region
    uint32_t removable = 0;                       // planned (every sample proven removable without side effects)
    uint32_t skippedUnsafe = 0;                   // no dig sequence stays clear of the neighbouring ground
    uint32_t touching = 0;                        // floaters that touch other ground diagonally (planned like any other)
    uint32_t skippedTouching = 0;                 // of those, the ones skipped as unsafe
  };

  FloaterPlan PlanFloaterRemoval(World& world, uint32_t maxSize, FloaterRegion const& region);
}
