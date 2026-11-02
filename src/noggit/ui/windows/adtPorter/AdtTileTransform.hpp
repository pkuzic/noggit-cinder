// This file is part of Noggit Cinder, licensed under GNU General Public License (version 3).
#pragma once

// Raw, byte-level ADT tile relocation shared by the ADT Porting Tool and the ADT Tile Mover.
//
// The ADT is edited IN PLACE (no re-serialisation through MapTile::save), so every chunk the
// file already carries survives byte-for-byte: heights/normals, texture layers + alphamaps,
// holes, MCCV, area ids, MCLQ (vanilla) or MH2O water, shadows, MTXF/MFBO. Only the fields
// that encode an absolute world position are rewritten:
//   * MCNK header position (xpos/zpos),
//   * MCSE sound-emitter positions (28-byte WotLK and 52-byte vanilla entries),
//   * MDDF doodad / MODF WMO placement positions (+ MODF extents),
// and every MDDF/MODF unique id is remapped to a fresh one.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Noggit::AdtTileTransform
{
  struct Stats
  {
    int models = 0;
    int wmos = 0;
    int sound_emitters = 0;
  };

  // Shifts the tile stored in `bytes` from tile (source_x, source_z) to
  // (destination_x, destination_z). `uid_map` maps old unique ids to new ones and is meant to
  // be SHARED across every tile of one block operation, so an object referenced by two moved
  // neighbour tiles (border twin) stays one object with one id. Unmapped ids get ++next_uid.
  bool shiftTile(std::vector<char>& bytes, int source_x, int source_z,
                 int destination_x, int destination_z,
                 std::unordered_map<std::uint32_t, std::uint32_t>& uid_map,
                 std::uint32_t& next_uid, Stats& stats, std::string& error);

  // Raises `max_uid` to the highest MDDF/MODF unique id in the ADT. Returns false on a
  // structurally broken file (max_uid is left as far as it got).
  bool collectMaxUid(std::vector<char> const& bytes, std::uint32_t& max_uid, std::string& error);
}
