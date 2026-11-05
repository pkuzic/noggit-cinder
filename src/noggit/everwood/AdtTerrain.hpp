// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// The raw ADT ground the voxel field seeds from: the game client's terrain_height_at (benilla-formats
// terrain.rs + benilla-adt), read straight from the ADT bytes, so a column seeds to exactly the same
// float as in the game. It deliberately does not go through Noggit's in-memory MapChunk: that holds
// positions rebased to Noggit's axes, and unsaved terrain edits the game would never see.
//
// Pure C++, no Noggit or Qt headers. WoW axes (x north, y west, z up).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Noggit::Everwood::Adt
{
  constexpr float TILE_SIZE = 533.3333f;
  constexpr float CHUNK_SIZE = TILE_SIZE / 16.0f;
  constexpr float UNIT_SIZE = CHUNK_SIZE / 8.0f;

  struct Chunk
  {
    std::array<std::array<float, 3>, 145> positions;   // stride-17 interleave, as adt_to_tile_mesh builds it
    uint16_t holes = 0;

    // ChunkMesh::height_at: the covering fan triangle, false off the footprint or in a hole.
    bool HeightAt(float x, float y, float& h) const;
  };

  struct Tile
  {
    std::vector<Chunk> chunks;                         // file order, chunks without MCVT are skipped

    // terrain_height_at
    bool HeightAt(float x, float y, float& h) const;
  };

  // parse_adt + read_mcnk + adt_to_tile_mesh, heights only. false = no terrain chunks in the file.
  bool ParseTile(void const* data, size_t size, Tile& out);

  // benilla-wdt world_to_tile, clamped to 0..=63.
  void WorldToTile(float x, float y, int& tileX, int& tileY);

  bool TriangleZAt(std::array<float, 3> const& a, std::array<float, 3> const& b, std::array<float, 3> const& c
                   , float x, float y, float& z);
}
