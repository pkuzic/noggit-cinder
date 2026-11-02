// This file is part of Noggit Cinder, licensed under GNU General Public License (version 3).
#include "AdtTileTransform.hpp"

#include <noggit/MapHeaders.h>

#include <algorithm>
#include <cstring>

namespace
{
  struct RawChunkHeader
  {
    std::uint32_t magic;
    std::uint32_t size;
  };

  template<typename Bytes, typename Visitor>
  bool visitTopLevel(Bytes& bytes, Visitor visitor, std::string& error)
  {
    std::size_t position = 0;
    while (position + sizeof(RawChunkHeader) <= bytes.size())
    {
      RawChunkHeader header;
      std::memcpy(&header, bytes.data() + position, sizeof(header));
      std::size_t const data_position = position + sizeof(RawChunkHeader);
      std::size_t const end = data_position + header.size;
      if (end > bytes.size())
      {
        error = "ADT contains a truncated top-level chunk.";
        return false;
      }
      visitor(header, bytes.data() + data_position, header.size);
      position = end;
    }
    // Fewer than 8 stray bytes cannot be a chunk: stock tiles end in such padding. Left untouched.
    return true;
  }

  // MCSE lives inside an MCNK. ofsSndEmitters is relative to the MCNK magic, i.e. 8 bytes
  // before `mcnk_data`. Entry stride is derived from the sub-chunk size: 28 bytes (WotLK:
  // id, pos, size) or 52 bytes (vanilla: soundPointID, soundNameID, pos, ...). Positions use
  // the MCNK convention: pos[0] = ZEROPOINT - world_z, pos[1] = ZEROPOINT - world_x.
  int shiftSoundEmitters(MapChunkHeader const& header, char* mcnk_data, std::uint32_t mcnk_size,
                         float dx, float dz)
  {
    if (!header.nSndEmitters || header.ofsSndEmitters < 8)
      return 0;
    std::size_t const sub = header.ofsSndEmitters - 8;
    if (sub + 8 > mcnk_size)
      return 0;
    RawChunkHeader mcse;
    std::memcpy(&mcse, mcnk_data + sub, sizeof(mcse));
    if (mcse.magic != 'MCSE' || sub + 8 + mcse.size > mcnk_size
        || mcse.size % header.nSndEmitters != 0)
      return 0;
    std::uint32_t const stride = mcse.size / header.nSndEmitters;
    std::size_t pos_offset;
    if (stride == 28)
      pos_offset = 4;
    else if (stride == 52)
      pos_offset = 8;
    else
      return 0;
    char* entries = mcnk_data + sub + 8;
    for (std::uint32_t i = 0; i < header.nSndEmitters; ++i)
    {
      float pos[3];
      std::memcpy(pos, entries + i * stride + pos_offset, sizeof(pos));
      pos[0] -= dz;
      pos[1] -= dx;
      std::memcpy(entries + i * stride + pos_offset, pos, sizeof(pos));
    }
    return static_cast<int>(header.nSndEmitters);
  }
}

namespace Noggit::AdtTileTransform
{
  bool shiftTile(std::vector<char>& bytes, int source_x, int source_z,
                 int destination_x, int destination_z,
                 std::unordered_map<std::uint32_t, std::uint32_t>& uid_map,
                 std::uint32_t& next_uid, Stats& stats, std::string& error)
  {
    float const dx = static_cast<float>(destination_x - source_x) * TILESIZE;
    float const dz = static_cast<float>(destination_z - source_z) * TILESIZE;
    auto remap = [&](std::uint32_t old_uid)
    {
      auto it = uid_map.find(old_uid);
      if (it != uid_map.end())
        return it->second;
      std::uint32_t const fresh = ++next_uid;
      uid_map.emplace(old_uid, fresh);
      return fresh;
    };

    int mcnk_count = 0;
    bool valid_version = false;
    bool const okay = visitTopLevel(bytes,
      [&](RawChunkHeader const& chunk, char* data, std::uint32_t size)
      {
        if (chunk.magic == 'MVER' && size == 4)
        {
          std::uint32_t version;
          std::memcpy(&version, data, 4);
          // 19 = Turtle-edited tiles (Stormwind area): the same layout as 18, version kept as is.
          valid_version = version == 18 || version == 19;
        }
        else if (chunk.magic == 'MCNK' && size >= sizeof(MapChunkHeader))
        {
          MapChunkHeader header;
          std::memcpy(&header, data, sizeof(header));
          // xpos = ZEROPOINT - world_x, zpos = ZEROPOINT - world_z (see MapChunk save).
          header.xpos -= dx;
          header.zpos -= dz;
          std::memcpy(data, &header, sizeof(header));
          stats.sound_emitters += shiftSoundEmitters(header, data, size, dx, dz);
          ++mcnk_count;
        }
        else if (chunk.magic == 'MDDF' && size % sizeof(ENTRY_MDDF) == 0)
        {
          int const count = static_cast<int>(size / sizeof(ENTRY_MDDF));
          for (int i = 0; i < count; ++i)
          {
            ENTRY_MDDF entry;
            std::memcpy(&entry, data + i * sizeof(ENTRY_MDDF), sizeof(entry));
            entry.pos[0] += dx;
            entry.pos[2] += dz;
            entry.uniqueID = remap(entry.uniqueID);
            std::memcpy(data + i * sizeof(ENTRY_MDDF), &entry, sizeof(entry));
          }
          stats.models += count;
        }
        else if (chunk.magic == 'MODF' && size % sizeof(ENTRY_MODF) == 0)
        {
          int const count = static_cast<int>(size / sizeof(ENTRY_MODF));
          for (int i = 0; i < count; ++i)
          {
            ENTRY_MODF entry;
            std::memcpy(&entry, data + i * sizeof(ENTRY_MODF), sizeof(entry));
            entry.pos[0] += dx;
            entry.pos[2] += dz;
            entry.extents[0].x += dx;
            entry.extents[0].z += dz;
            entry.extents[1].x += dx;
            entry.extents[1].z += dz;
            entry.uniqueID = remap(entry.uniqueID);
            std::memcpy(data + i * sizeof(ENTRY_MODF), &entry, sizeof(entry));
          }
          stats.wmos += count;
        }
      }, error);
    if (!okay)
      return false;
    if (!valid_version)
    {
      error = "Only ADT versions 18 and 19 (vanilla / Turtle / TBC / WotLK) are supported.";
      return false;
    }
    if (mcnk_count != 256)
    {
      error = "Source ADT contains " + std::to_string(mcnk_count) + " MCNK chunks instead of 256.";
      return false;
    }
    return true;
  }

  bool collectMaxUid(std::vector<char> const& bytes, std::uint32_t& max_uid, std::string& error)
  {
    return visitTopLevel(bytes,
      [&](RawChunkHeader const& chunk, char const* data, std::uint32_t size)
      {
        std::size_t stride = 0;
        if (chunk.magic == 'MDDF' && size % sizeof(ENTRY_MDDF) == 0)
          stride = sizeof(ENTRY_MDDF);
        else if (chunk.magic == 'MODF' && size % sizeof(ENTRY_MODF) == 0)
          stride = sizeof(ENTRY_MODF);
        if (!stride)
          return;
        for (std::size_t offset = 0; offset < size; offset += stride)
        {
          std::uint32_t uid;
          std::memcpy(&uid, data + offset + 4, 4); // uniqueID follows nameID in both
          max_uid = std::max(max_uid, uid);
        }
      }, error);
  }
}
