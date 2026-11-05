// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/everwood/AdtTerrain.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Noggit::Everwood::Adt
{
  namespace
  {
    // terrain.rs snap_to_lattice: X/Y snap so shared edge verts coincide.
    float snapToLattice(float coord)
    {
      double const mapCenter = 32.0 * 1600.0 / 3.0;
      double const unit = (1600.0 / 3.0) / 128.0;
      double const idx = std::round((mapCenter - double(coord)) / unit);
      return float(mapCenter - idx * unit);
    }

    template <typename T>
    bool readAt(unsigned char const* data, size_t size, size_t at, T& out)
    {
      if (at + sizeof(T) > size)
        return false;
      std::memcpy(&out, data + at, sizeof(T));
      return true;
    }

    // read_mcnk `locate`: an offset may count from the MCNK magic or from its data, so the reversed
    // magic is checked at both.
    bool locateMcvt(unsigned char const* mcnk, size_t size, uint32_t ofs, size_t& start, size_t& len)
    {
      if (ofs == 0)
        return false;
      size_t const candidates[2] = { size_t(ofs), ofs >= 8 ? size_t(ofs) - 8 : size_t(-1) };
      for (size_t hdr : candidates)
      {
        if (hdr == size_t(-1) || hdr + 8 > size)
          continue;
        if (std::memcmp(mcnk + hdr, "TVCM", 4) == 0)
        {
          uint32_t n = 0;
          readAt(mcnk, size, hdr + 4, n);
          start = hdr + 8;
          len = std::min<size_t>(n, size - start);
          return true;
        }
      }
      return false;
    }

    bool readChunk(unsigned char const* mcnk, size_t size, Chunk& out)
    {
      uint32_t ofsHeight = 0;
      uint16_t holes = 0;
      float base[3];
      if (!readAt(mcnk, size, 0x14, ofsHeight) || !readAt(mcnk, size, 0x3C, holes)
          || !readAt(mcnk, size, 0x68, base[0]) || !readAt(mcnk, size, 0x6C, base[1]) || !readAt(mcnk, size, 0x70, base[2]))
        return false;

      size_t start = 0, len = 0;
      if (!locateMcvt(mcnk, size, ofsHeight, start, len) || len < 145 * 4)
        return false;

      float heights[145];
      std::memcpy(heights, mcnk + start, sizeof(heights));

      out.holes = holes;
      float const wx = base[0], wy = base[1], wz = base[2];
      size_t n = 0;
      for (uint32_t row = 0; row < 9; ++row)
      {
        for (uint32_t col = 0; col < 9; ++col)
        {
          float const h = heights[row * 17 + col];
          out.positions[n++] = { snapToLattice(wx - float(row) * UNIT_SIZE)
                               , snapToLattice(wy - float(col) * UNIT_SIZE)
                               , wz + h };
        }
        if (row < 8)
        {
          for (uint32_t col = 0; col < 8; ++col)
          {
            float const h = heights[row * 17 + 9 + col];
            out.positions[n++] = { wx - (float(row) * UNIT_SIZE + UNIT_SIZE / 2.0f)
                                 , wy - (float(col) * UNIT_SIZE + UNIT_SIZE / 2.0f)
                                 , wz + h };
          }
        }
      }
      return true;
    }
  }

  bool TriangleZAt(std::array<float, 3> const& a, std::array<float, 3> const& b, std::array<float, 3> const& c
                   , float x, float y, float& z)
  {
    float const det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
    if (std::fabs(det) < 1.0e-9f)
      return false;
    float const l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det;
    float const l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det;
    float const l3 = 1.0f - l1 - l2;
    if (l1 < 0.0f || l2 < 0.0f || l3 < 0.0f)
      return false;
    z = l1 * a[2] + l2 * b[2] + l3 * c[2];
    return true;
  }

  bool Chunk::HeightAt(float x, float y, float& h) const
  {
    auto const& nw = positions[0];
    float const south = (nw[0] - x) / TILE_SIZE * 16.0f;
    float const east = (nw[1] - y) / TILE_SIZE * 16.0f;
    if (!(south >= 0.0f && south <= 1.0f) || !(east >= 0.0f && east <= 1.0f))
      return false;

    uint32_t const row = std::min<uint32_t>(uint32_t(south * 8.0f), 7);
    uint32_t const col = std::min<uint32_t>(uint32_t(east * 8.0f), 7);
    if (holes & (uint16_t(1) << ((row >> 1) * 4 + (col >> 1))))
      return false;

    size_t const tl = row * 17 + col, tr = tl + 1, bl = tl + 17, br = tl + 18, ctr = row * 17 + 9 + col;
    auto const& p = positions;
    return TriangleZAt(p[ctr], p[tl], p[bl], x, y, h)
        || TriangleZAt(p[ctr], p[bl], p[br], x, y, h)
        || TriangleZAt(p[ctr], p[br], p[tr], x, y, h)
        || TriangleZAt(p[ctr], p[tr], p[tl], x, y, h);
  }

  bool Tile::HeightAt(float x, float y, float& h) const
  {
    // chunk_at: the MCNK of a full tile, by arithmetic. A miss falls through to the linear walk.
    if (chunks.size() == 256)
    {
      auto const& nw = chunks[0].positions[0];
      float const row = (nw[0] - x) / CHUNK_SIZE;
      float const col = (nw[1] - y) / CHUNK_SIZE;
      if (row >= 0.0f && row < 16.0f && col >= 0.0f && col < 16.0f)
        return chunks[size_t(row) * 16 + size_t(col)].HeightAt(x, y, h);
    }
    for (Chunk const& c : chunks)
      if (c.HeightAt(x, y, h))
        return true;
    return false;
  }

  bool ParseTile(void const* dataPtr, size_t size, Tile& out)
  {
    auto const* data = static_cast<unsigned char const*>(dataPtr);
    out.chunks.clear();
    size_t at = 0;
    while (at + 8 <= size)
    {
      uint32_t len = 0;
      std::memcpy(&len, data + at + 4, 4);
      size_t const payload = at + 8;
      size_t const avail = std::min<size_t>(len, size - payload);
      if (std::memcmp(data + at, "KNCM", 4) == 0)
      {
        Chunk c;
        if (readChunk(data + payload, avail, c))
          out.chunks.push_back(c);
      }
      at = payload + avail;
      if (avail < len)
        break;
    }
    return !out.chunks.empty();
  }

  void WorldToTile(float x, float y, int& tileX, int& tileY)
  {
    float const offset = 32.0f * TILE_SIZE;
    // `as u32` saturates a negative to 0.
    float const fx = (offset - y) / TILE_SIZE;
    float const fy = (offset - x) / TILE_SIZE;
    tileX = fx <= 0.0f ? 0 : std::min(int(fx), 63);
    tileY = fy <= 0.0f ? 0 : std::min(int(fy), 63);
  }
}
