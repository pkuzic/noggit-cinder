// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/everwood/EverwoodLayer.hpp>

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/World.h>
#include <noggit/texture_set.hpp>
#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <opengl/texture.hpp>
#include <math/frustum.hpp>
#include <revision.h>
#include <ClientFile.hpp>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtGui/QOpenGLContext>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>

namespace Noggit::Everwood
{
  namespace
  {
    using namespace Voxel;

    constexpr int MASK_SIZE = 1072;         // a tile is 1066.7 lattice cells, + a margin; a multiple of 4
    constexpr int MASK_UNIT = 16;           // texture unit of the terrain carve mask
    constexpr int FIRST_TEXTURE_UNIT = 5;   // the terrain shader's own 11 layer samplers
    constexpr int NUM_TEXTURE_UNITS = 11;
    constexpr float AIM_STEP = 0.5f;
    constexpr int LAYER_SPECULAR = 1 << 20; // on a mesh layer id: keep the texture's specular mask

    std::string toHex(uint8_t const* data, size_t n)
    {
      static char const digits[] = "0123456789abcdef";
      std::string s;
      s.reserve(n * 2);
      for (size_t i = 0; i < n; ++i)
      {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 0xf]);
      }
      return s;
    }

    bool fromHex(std::string const& s, uint8_t* out, size_t n)
    {
      if (s.size() != n * 2)
        return false;
      auto digit = [](char c) -> int
      {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      for (size_t i = 0; i < n; ++i)
      {
        int const a = digit(s[i * 2]), b = digit(s[i * 2 + 1]);
        if (a < 0 || b < 0)
          return false;
        out[i] = uint8_t((a << 4) | b);
      }
      return true;
    }

    // One line of the .mvoxj / export "edits" array. "wire" is authoritative, the rest is for people.
    std::string editLine(Edit const& e)
    {
      auto const wire = ToWire3(e);
      char buf[320];
      std::snprintf(buf, sizeof(buf),
                    "{\"seq\":%u,\"wire\":\"%s\",\"stroke\":%u,\"nonce\":%u,\"map\":%u,\"x\":%.9g,\"y\":%.9g,\"z\":%.9g,\"radius\":%.9g,\"mode\":%u,\"strength\":%u,\"param\":%u}",
                    e.seq, toHex(wire.data(), wire.size()).c_str(), unsigned(e.stroke), unsigned(e.nonce), unsigned(e.map),
                    double(e.pos[0]), double(e.pos[1]), double(e.pos[2]), double(e.radius),
                    unsigned(e.mode), unsigned(e.strength), unsigned(e.param));
      return buf;
    }

    std::string jsonString(std::string const& s)
    {
      std::string out = "\"";
      for (char c : s)
      {
        if (c == '"' || c == '\\')
          out.push_back('\\');
        out.push_back(c);
      }
      out.push_back('"');
      return out;
    }

    float clamp01(float v)
    {
      return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    // A point of a chunk's 145-vertex grid: the three vertices of the terrain's triangle fan around each cell's
    // centre vertex, and their weights. u, w run over the chunk, 0..1.
    struct FanPoint
    {
      int index[3];
      float weight[3];
    };

    FanPoint fanPoint(float u, float w)
    {
      float const cu = std::clamp(u * 8.f, 0.f, 8.f), cw = std::clamp(w * 8.f, 0.f, 8.f);
      int const ci = std::min(int(cu), 7), cj = std::min(int(cw), 7);
      float const a = cu - float(ci), b = cw - float(cj);
      int const tl = cj * 17 + ci, tr = tl + 1, bl = tl + 17, br = tl + 18, ctr = cj * 17 + 9 + ci;
      if (b <= a && b <= 1.f - a)
        return { { ctr, tr, tl }, { 2.f * b, a - b, 1.f - a - b } };
      if (a >= b && a >= 1.f - b)
        return { { ctr, br, tr }, { 2.f * (1.f - a), b - (1.f - a), a - b } };
      if (b >= a && b >= 1.f - a)
        return { { ctr, br, bl }, { 2.f * (1.f - b), a - (1.f - b), b - a } };
      return { { ctr, bl, tl }, { 2.f * a, b - a, 1.f - a - b } };
    }

    float smoothstep01(float a, float b, float v)
    {
      float const t = clamp01((v - a) / (b - a));
      return t * t * (3.f - 2.f * t);
    }
  }

  glm::vec3 wowToNoggit(glm::vec3 const& wow)
  {
    return { ZEROPOINT - wow.y, wow.z, ZEROPOINT - wow.x };
  }

  glm::vec3 noggitToWow(glm::vec3 const& n)
  {
    return { ZEROPOINT - n.z, ZEROPOINT - n.x, n.y };
  }

  EverwoodLayer::EverwoodLayer(::World* world, std::string projectPath, std::string mapDirectory, uint16_t mapId)
    : _world(world)
    , _projectPath(std::move(projectPath))
    , _mapDirectory(std::move(mapDirectory))
    , _mapId(mapId)
    , _field([this](float x, float y, float& h) { return seedHeight(x, y, h); })
  {
  }

  EverwoodLayer::~EverwoodLayer() = default;

  // ------------------------------------------------------------------------------------------- seeding
  void EverwoodLayer::readAdt(int tileX, int tileY, std::vector<char>& out)
  {
    std::string const path = "World\\Maps\\" + _mapDirectory + "\\" + _mapDirectory + "_"
      + std::to_string(tileX) + "_" + std::to_string(tileY) + ".adt";
    auto* clientData = Noggit::Application::NoggitApplication::instance()->clientData();
    BlizzardArchive::Listfile::FileKey const key(path);
    if (!clientData->exists(key))
      return;
    BlizzardArchive::ClientFile file(key, clientData);
    if (file.isEof() || file.getSize() == 0)
      return;
    out.assign(file.getBuffer(), file.getBuffer() + file.getSize());
  }

  Adt::Tile const* EverwoodLayer::tileAt(int tileX, int tileY)
  {
    int const key = tileX * 64 + tileY;
    auto it = _tiles.find(key);
    if (it != _tiles.end())
      return it->second.get();

    std::shared_ptr<Adt::Tile> tile;
    std::vector<char> bytes;
    readAdt(tileX, tileY, bytes);
    if (!bytes.empty())
    {
      auto parsed = std::make_shared<Adt::Tile>();
      if (Adt::ParseTile(bytes.data(), bytes.size(), *parsed))
        tile = std::move(parsed);
      else
        LogError << "Everwood: no terrain chunks in tile " << tileX << "_" << tileY << std::endl;
    }
    _tiles.emplace(key, tile);
    return tile.get();
  }

  bool EverwoodLayer::seedHeight(float x, float y, float& h)
  {
    int tx, ty;
    Adt::WorldToTile(x, y, tx, ty);
    Adt::Tile const* tile = tileAt(tx, ty);
    return tile && tile->HeightAt(x, y, h);
  }

  bool EverwoodLayer::rawGround(float wowX, float wowY, float& height)
  {
    return seedHeight(wowX, wowY, height);
  }

  // ----------------------------------------------------------------------------------------- the journal
  uint16_t EverwoodLayer::beginStroke()
  {
    // CONTRACT 9.4: starting a new stroke makes the previously undone strokes non-redoable.
    _entries.erase(std::remove_if(_entries.begin(), _entries.end(), [](Entry const& e) { return e.undone; }), _entries.end());

    uint16_t const id = _nextStroke;
    _nextStroke = _nextStroke == 65535 ? 1 : uint16_t(_nextStroke + 1);
    return id;
  }

  void EverwoodLayer::addEdit(Edit edit, uint16_t stroke)
  {
    edit.seq = _nextSeq;
    edit.nonce = _nextNonce;
    edit.stroke = stroke;
    edit.map = _mapId;
    if (!edit.IsValid() || !edit.IsApplied())
      return;

    ++_nextSeq;
    _nextNonce = uint16_t(_nextNonce + 1);
    _entries.push_back({ edit, false });
    _journal.Add(edit);

    std::vector<Key> added;
    std::vector<Key> const changed = _field.ApplyEdit(edit, &added);
    noteChanged(changed, &added);
    flushColumns();
  }

  void EverwoodLayer::endStroke()
  {
    save();
  }

  void EverwoodLayer::rebuildJournal()
  {
    _journal.Clear();
    for (Entry const& e : _entries)
      if (!e.undone)
        _journal.Add(e.edit);
  }

  void EverwoodLayer::setStrokeUndone(uint16_t stroke, bool undone)
  {
    std::vector<Key> touched;
    for (Entry& e : _entries)
    {
      if (e.edit.stroke != stroke || e.undone == undone)
        continue;
      e.undone = undone;
      for (Key const& k : e.edit.TouchedChunks())
        touched.push_back(k);
    }
    if (touched.empty())
      return;

    // The closure on both sides of the change: a chunk may be linked to the stroke only through an edit of
    // the stroke itself (undo) or only once it is back (redo).
    std::set<Key> closure = _journal.ReadClosure(touched);
    rebuildJournal();
    std::set<Key> const after = _journal.ReadClosure(touched);
    closure.insert(after.begin(), after.end());

    std::vector<Key> added;
    if (!undone)
      for (Key const& k : closure)
        _field.Activate(k, &added);

    std::vector<Key> keys;
    for (Key const& k : closure)
      if (_field.IsActive(k))
        keys.push_back(k);
    _field.Rebuild(keys, _journal.EditsFor(keys));

    noteChanged(keys, &added);
    flushColumns();
    save();
  }

  size_t EverwoodLayer::strokeCount() const
  {
    std::set<uint16_t> strokes;
    for (Edit const& e : _journal.Edits())
      strokes.insert(e.stroke);
    return strokes.size();
  }

  void EverwoodLayer::noteChanged(std::vector<Key> const& keys, std::vector<Key> const* added)
  {
    auto dirtyAround = [this](Key const& k)
    {
      _dirtyColumns.insert(ColKey{ k[0], k[1] });
      // A chunk's mesh window reads one sample of each neighbour.
      for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
          {
            Key const n{ k[0] + dx, k[1] + dy, k[2] + dz };
            if (_field.IsActive(n))
              _dirtyMeshes.insert(n);
          }
    };
    for (Key const& k : keys)
      dirtyAround(k);
    if (added)
      for (Key const& k : *added)
        dirtyAround(k);
  }

  // ------------------------------------------------------------------------- what is edited, per column
  void EverwoodLayer::recomputeColumn(ColKey const& col)
  {
    ColumnState& state = _columns[col];
    state.changed.assign(size_t(C) * C, 0);
    state.any = false;
    state.surfaceOk = false;

    auto const seed = _field.Seed(col);
    int32_t lo, hi;
    if (seed->SurfaceChunks(lo, hi))
    {
      state.surfaceOk = true;
      for (int32_t kz = lo; kz <= hi; ++kz)
        if (!_field.IsActive(Key{ col[0], col[1], kz }))
          state.surfaceOk = false;
    }

    auto const* wanted = _field.WantedInColumn(col);
    if (!wanted)
      return;

    int32_t const bx = col[0] * C, by = col[1] * C;
    for (int32_t kz : *wanted)
    {
      Key const key{ col[0], col[1], kz };
      auto const it = _field.ChunksRef().find(key);
      if (it == _field.ChunksRef().end())
        continue;
      Density const& d = it->second;
      int32_t const bz = kz * C;
      for (int32_t y = 0; y < C; ++y)
        for (int32_t x = 0; x < C; ++x)
        {
          if (state.changed[size_t(x) + size_t(y) * C])
            continue;
          float const h = seed->HeightLat(bx + x, by + y);
          for (int32_t z = 0; z < C; ++z)
            if (d[owned_index(x, y, z)] != seed_density(h, bz + z))
            {
              state.changed[size_t(x) + size_t(y) * C] = 1;
              state.any = true;
              break;
            }
        }
    }
  }

  bool EverwoodLayer::columnChangedAt(int32_t ix, int32_t iy) const
  {
    int32_t const kx = chunk_of(ix), ky = chunk_of(iy);
    auto it = _columns.find(ColKey{ kx, ky });
    if (it == _columns.end() || !it->second.any)
      return false;
    return it->second.changed[size_t(ix - kx * C) + size_t(iy - ky * C) * C] != 0;
  }

  bool EverwoodLayer::surfaceOkAt(int32_t ix, int32_t iy) const
  {
    auto it = _columns.find(ColKey{ chunk_of(ix), chunk_of(iy) });
    return it != _columns.end() && it->second.surfaceOk;
  }

  EverwoodLayer::TileMask& EverwoodLayer::maskFor(int tileX, int tileY)
  {
    TileMask& mask = _masks[tileX * 64 + tileY];
    if (mask.pixels.empty())
    {
      mask.pixels.assign(size_t(MASK_SIZE) * MASK_SIZE, 0);
      // Tile (tileX, tileY) covers WoW y from ZEROPOINT - (tileX + 1) * TILESIZE, WoW x likewise from tileY.
      mask.origin.x = int(std::floor((ZEROPOINT - float(tileY + 1) * TILESIZE) / VOXEL_SIZE)) - 2;
      mask.origin.y = int(std::floor((ZEROPOINT - float(tileX + 1) * TILESIZE) / VOXEL_SIZE)) - 2;
    }
    return mask;
  }

  void EverwoodLayer::setMaskCell(int32_t i, int32_t j, bool hidden)
  {
    // Both tiles of a cell on a tile seam need it: each draws its own side of the seam.
    float const xs[2] = { lattice_to_wow(i), lattice_to_wow(i + 1) };
    float const ys[2] = { lattice_to_wow(j), lattice_to_wow(j + 1) };
    int last = -1;
    for (float x : xs)
      for (float y : ys)
      {
        int tx, ty;
        Adt::WorldToTile(x, y, tx, ty);
        int const key = tx * 64 + ty;
        if (key == last)
          continue;
        last = key;
        if (!hidden && _masks.find(key) == _masks.end())
          continue;
        TileMask& mask = maskFor(tx, ty);
        int const u = i - mask.origin.x, v = j - mask.origin.y;
        if (u < 0 || v < 0 || u >= MASK_SIZE || v >= MASK_SIZE)
          continue;
        uint8_t& px = mask.pixels[size_t(u) + size_t(v) * MASK_SIZE];
        uint8_t const value = hidden ? 255 : 0;
        if (px == value)
          continue;
        px = value;
        mask.hidden += hidden ? 1 : size_t(-1);
        mask.dirtyLo = mask.dirtyHi < 0 ? v : std::min(mask.dirtyLo, v);
        mask.dirtyHi = std::max(mask.dirtyHi, v);
      }
  }

  void EverwoodLayer::writeMaskAround(ColKey const& col)
  {
    // A cell is hidden when one of its four corner lattice columns changed and the mesh exists under all of
    // them. Column (kx, ky) feeds the cells from its first lattice line - 1 to its last.
    int32_t const x0 = col[0] * C - 1, y0 = col[1] * C - 1;
    for (int32_t j = y0; j < y0 + C + 1; ++j)
      for (int32_t i = x0; i < x0 + C + 1; ++i)
      {
        bool const anyChanged = columnChangedAt(i, j) || columnChangedAt(i + 1, j)
          || columnChangedAt(i, j + 1) || columnChangedAt(i + 1, j + 1);
        bool const hidden = anyChanged && surfaceOkAt(i, j) && surfaceOkAt(i + 1, j)
          && surfaceOkAt(i, j + 1) && surfaceOkAt(i + 1, j + 1);
        setMaskCell(i, j, hidden);
      }
  }

  void EverwoodLayer::flushColumns()
  {
    for (ColKey const& col : _dirtyColumns)
      recomputeColumn(col);
    for (ColKey const& col : _dirtyColumns)
      writeMaskAround(col);
    _dirtyColumns.clear();

    // The box the cursor looks for the voxel surface in.
    _editedMin = glm::vec3(std::numeric_limits<float>::max());
    _editedMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (auto const& kv : _field.ChunksRef())
    {
      auto const column = _columns.find(ColKey{ kv.first[0], kv.first[1] });
      if (column == _columns.end() || !column->second.any)
        continue;
      glm::vec3 const low(float(kv.first[0]) * CHUNK_YARDS, float(kv.first[1]) * CHUNK_YARDS, float(kv.first[2]) * CHUNK_YARDS);
      _editedMin = glm::min(_editedMin, low);
      _editedMax = glm::max(_editedMax, low + glm::vec3(CHUNK_YARDS));
    }
  }

  size_t EverwoodLayer::hiddenCellCount() const
  {
    size_t n = 0;
    for (auto const& kv : _masks)
      n += kv.second.hidden;
    return n;
  }

  uint64_t EverwoodLayer::stateHash()
  {
    std::vector<Key> keys;
    for (auto const& kv : _field.ChunksRef())
    {
      Key const& k = kv.first;
      auto const seed = _field.Seed(ColKey{ k[0], k[1] });
      int32_t const bx = k[0] * C, by = k[1] * C, bz = k[2] * C;
      bool differs = false;
      for (int32_t z = 0; z < C && !differs; ++z)
        for (int32_t y = 0; y < C && !differs; ++y)
          for (int32_t x = 0; x < C; ++x)
            if (kv.second[owned_index(x, y, z)] != seed->Value(bx + x, by + y, bz + z))
            {
              differs = true;
              break;
            }
      if (differs)
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    uint64_t h = 0xcbf29ce484222325ull;
    for (Key const& k : keys)
    {
      for (int32_t a : k)
        h = Fnv(&a, 4, h);
      Density const& d = _field.ChunksRef().at(k);
      h = Fnv(d.data(), d.size(), h);
    }
    return h;
  }

  // ------------------------------------------------------------------------------------------------ aim
  float EverwoodLayer::aimDensity(glm::vec3 const& p)
  {
    Key const k{ int32_t(std::floor(p.x / CHUNK_YARDS)), int32_t(std::floor(p.y / CHUNK_YARDS)), int32_t(std::floor(p.z / CHUNK_YARDS)) };
    if (_field.IsActive(k))
      return _field.DensityAt(p.x, p.y, p.z);
    float h;
    if (seedHeight(p.x, p.y, h))
      return (p.z - (h + SEED_LIFT)) / VOXEL_SIZE;
    return 1.f;
  }

  std::optional<AimHit> EverwoodLayer::raycast(glm::vec3 const& origin, glm::vec3 const& dirIn, float maxDist)
  {
    float const len = glm::length(dirIn);
    if (len < 1e-6f)
      return std::nullopt;
    glm::vec3 const d = dirIn / len;
    float t = 0.f;
    return march(origin, d, t, maxDist);
  }

  std::optional<AimHit> EverwoodLayer::march(glm::vec3 const& origin, glm::vec3 const& d, float& tInOut, float tEnd)
  {
    auto at = [&](float t) { return origin + d * t; };

    if (aimDensity(at(tInOut)) < 0.f)
      return std::nullopt;   // the camera is under the ground: aim at nothing rather than at itself

    float prev = tInOut;
    for (float t = tInOut + AIM_STEP; t <= tEnd; t += AIM_STEP)
    {
      if (aimDensity(at(t)) < 0.f)
      {
        float a = prev, b = t;
        for (int i = 0; i < 12; ++i)
        {
          float const m = 0.5f * (a + b);
          if (aimDensity(at(m)) < 0.f)
            b = m;
          else
            a = m;
        }
        tInOut = b;
        glm::vec3 const p = at(b);
        float const h = VOXEL_SIZE;
        glm::vec3 g(aimDensity(p + glm::vec3(h, 0, 0)) - aimDensity(p - glm::vec3(h, 0, 0))
                    , aimDensity(p + glm::vec3(0, h, 0)) - aimDensity(p - glm::vec3(0, h, 0))
                    , aimDensity(p + glm::vec3(0, 0, h)) - aimDensity(p - glm::vec3(0, 0, h)));
        float const gl_ = glm::length(g);
        return AimHit{ p, gl_ > 1e-5f ? g / gl_ : glm::vec3(0, 0, 1) };
      }
      prev = t;
    }
    return std::nullopt;
  }

  bool EverwoodLayer::cellHiddenAt(float wowX, float wowY) const
  {
    int tx, ty;
    Adt::WorldToTile(wowX, wowY, tx, ty);
    auto it = _masks.find(tx * 64 + ty);
    if (it == _masks.end() || it->second.pixels.empty())
      return false;
    int const u = int(std::floor(wowX / VOXEL_SIZE)) - it->second.origin.x, v = int(std::floor(wowY / VOXEL_SIZE)) - it->second.origin.y;
    if (u < 0 || v < 0 || u >= MASK_SIZE || v >= MASK_SIZE)
      return false;
    return it->second.pixels[size_t(u) + size_t(v) * MASK_SIZE] != 0;
  }

  std::optional<glm::vec3> EverwoodLayer::surfaceUnderCursor(glm::vec3 const& originNoggit, glm::vec3 const& dirNoggit
                                                            , std::optional<glm::vec3> const& terrainHitNoggit)
  {
    if (_journal.Edits().empty() || _editedMax.x < _editedMin.x)
      return std::nullopt;

    glm::vec3 const origin = noggitToWow(originNoggit);
    glm::vec3 d = noggitToWow(originNoggit + dirNoggit) - origin;
    float const len = glm::length(d);
    if (len < 1e-6f)
      return std::nullopt;
    d /= len;

    // Only the box of what was edited: a long ray elsewhere must not read ADT after ADT from the disk.
    float tMin = 0.f, tMax = 20000.f;
    for (int a = 0; a < 3; ++a)
    {
      if (std::fabs(d[a]) < 1e-6f)
      {
        if (origin[a] < _editedMin[a] || origin[a] > _editedMax[a])
          return std::nullopt;
        continue;
      }
      float t0 = (_editedMin[a] - origin[a]) / d[a], t1 = (_editedMax[a] - origin[a]) / d[a];
      if (t0 > t1)
        std::swap(t0, t1);
      tMin = std::max(tMin, t0);
      tMax = std::min(tMax, t1);
    }
    if (tMin > tMax)
      return std::nullopt;

    float t = tMin;
    if (!march(origin, d, t, tMax + AIM_STEP))
      return std::nullopt;
    glm::vec3 const hit = origin + d * t;

    // The mesh is only drawn around changed columns; elsewhere the real terrain is what the mapper sees.
    int32_t const ix = int32_t(std::floor(hit.x / VOXEL_SIZE)), iy = int32_t(std::floor(hit.y / VOXEL_SIZE));
    bool nearChange = false;
    for (int32_t y = iy - 2; y <= iy + 3 && !nearChange; ++y)
      for (int32_t x = ix - 2; x <= ix + 3; ++x)
        if (columnChangedAt(x, y))
        {
          nearChange = true;
          break;
        }
    if (!nearChange)
      return std::nullopt;

    // Visible terrain in front of the surface still wins; terrain the mesh replaces does not count.
    if (terrainHitNoggit)
    {
      glm::vec3 const terrain = noggitToWow(*terrainHitNoggit);
      if (glm::length(terrain - origin) + 0.5f < t && !cellHiddenAt(terrain.x, terrain.y))
        return std::nullopt;
    }
    return wowToNoggit({ hit.x, hit.y, hit.z - RENDER_DROP });
  }

  // -------------------------------------------------------------------------------------------- storage
  std::string EverwoodLayer::filePath() const
  {
    return _projectPath + "/everwood/voxels/" + _mapDirectory + ".mvoxj";
  }

  bool EverwoodLayer::save()
  {
    QString const path = QString::fromStdString(filePath());
    QDir().mkpath(QFileInfo(path).absolutePath());

    std::ostringstream out;
    out << "{\n\"format\":\"mvoxj\",\n\"version\":1,\n\"wire\":\"EDIT3 CONTRACT v3.6\",\n"
        << "\"mapDirectory\":" << jsonString(_mapDirectory) << ",\n\"map\":" << _mapId << ",\n"
        << "\"nextSeq\":" << _nextSeq << ",\n\"nextNonce\":" << _nextNonce << ",\n\"nextStroke\":" << _nextStroke << ",\n"
        << "\"edits\":[\n";
    bool first = true;
    for (Edit const& e : _journal.Edits())
    {
      out << (first ? "" : ",\n") << editLine(e);
      first = false;
    }
    out << "\n]\n}\n";

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
      LogError << "Everwood: cannot write " << filePath() << std::endl;
      return false;
    }
    std::string const text = out.str();
    file.write(text.data(), qint64(text.size()));
    return file.commit();
  }

  bool EverwoodLayer::load()
  {
    QFile file(QString::fromStdString(filePath()));
    if (!file.exists())
      return true;
    if (!file.open(QIODevice::ReadOnly))
    {
      LogError << "Everwood: cannot read " << filePath() << std::endl;
      return false;
    }
    QJsonParseError error;
    QJsonDocument const doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (doc.isNull() || !doc.isObject() || doc.object().value("format").toString() != "mvoxj")
    {
      LogError << "Everwood: " << filePath() << " is not an mvoxj file: " << error.errorString().toStdString() << std::endl;
      return false;
    }
    QJsonObject const root = doc.object();
    if (root.value("version").toInt() > 1)
    {
      LogError << "Everwood: " << filePath() << " has a newer format version, ignored." << std::endl;
      return false;
    }

    _entries.clear();
    uint32_t maxSeq = 0;
    for (auto const& value : root.value("edits").toArray())
    {
      QJsonObject const o = value.toObject();
      uint8_t bytes[EDIT3_LEN];
      Edit e;
      if (!fromHex(o.value("wire").toString().toStdString(), bytes, EDIT3_LEN) || !FromWire3(bytes, e))
      {
        LogError << "Everwood: bad edit dropped from " << filePath() << std::endl;
        continue;
      }
      e.seq = uint32_t(o.value("seq").toDouble());
      maxSeq = std::max(maxSeq, e.seq);
      _entries.push_back({ e, false });
    }
    std::stable_sort(_entries.begin(), _entries.end(), [](Entry const& a, Entry const& b) { return a.edit.seq < b.edit.seq; });

    _nextSeq = std::max<uint32_t>(uint32_t(root.value("nextSeq").toDouble(1)), maxSeq + 1);
    _nextNonce = uint16_t(root.value("nextNonce").toInt(1));
    _nextStroke = uint16_t(root.value("nextStroke").toInt(1));
    if (_nextStroke == 0)
      _nextStroke = 1;

    reseed();
    Log << "Everwood: " << _journal.Edits().size() << " voxel edits loaded from " << filePath() << std::endl;
    return true;
  }

  void EverwoodLayer::reseed()
  {
    _tiles.clear();
    _field.Clear();
    _columns.clear();
    _dirtyColumns.clear();
    for (auto& kv : _masks)
    {
      kv.second.hidden = 0;
      std::fill(kv.second.pixels.begin(), kv.second.pixels.end(), uint8_t(0));
      kv.second.dirtyLo = 0;
      kv.second.dirtyHi = MASK_SIZE - 1;
    }
    for (auto& kv : _meshes)
      kv.second.indexCount = 0;

    rebuildJournal();
    std::vector<Key> added;
    std::vector<Key> changed;
    for (Edit const& e : _journal.Edits())
    {
      std::vector<Key> const c = _field.ApplyEdit(e, &added);
      changed.insert(changed.end(), c.begin(), c.end());
    }
    noteChanged(changed, &added);
    flushColumns();
  }

  std::string EverwoodLayer::exportEdits(std::string const& projectName)
  {
    QString const stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString const dir = QString::fromStdString(_projectPath + "/everwood/export");
    QDir().mkpath(dir);
    QString const path = dir + "/" + QString::fromStdString(_mapDirectory) + "-voxel-edits-" + stamp + ".mvoxexport";

    struct StrokeInfo
    {
      uint32_t firstSeq = 0, lastSeq = 0, edits = 0;
    };
    std::map<uint16_t, StrokeInfo> strokes;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    std::set<std::pair<int, int>> tiles;
    for (Edit const& e : _journal.Edits())
    {
      StrokeInfo& s = strokes[e.stroke];
      s.firstSeq = s.edits ? s.firstSeq : e.seq;
      s.lastSeq = e.seq;
      ++s.edits;
      for (int a = 0; a < 3; ++a)
      {
        lo[a] = std::min(lo[a], e.pos[a] - e.radius);
        hi[a] = std::max(hi[a], e.pos[a] + e.radius);
      }
      Key l, h;
      e.Bbox(l, h);
      for (int32_t kx = chunk_of(l[0]); kx <= chunk_of(h[0]); ++kx)
        for (int32_t ky = chunk_of(l[1]); ky <= chunk_of(h[1]); ++ky)
        {
          int tx, ty;
          Adt::WorldToTile((float(kx) + 0.5f) * CHUNK_YARDS, (float(ky) + 0.5f) * CHUNK_YARDS, tx, ty);
          tiles.insert({ tx, ty });
        }
    }

    std::ostringstream out;
    out << "{\n\"format\":\"mvoxexport\",\n\"version\":1,\n\"wire\":\"EDIT3 CONTRACT v3.6\",\n"
        << "\"exportedAt\":" << jsonString(QDateTime::currentDateTime().toString(Qt::ISODate).toStdString()) << ",\n"
        << "\"noggitVersion\":" << jsonString(STRPRODUCTVER) << ",\n"
        << "\"project\":" << jsonString(projectName) << ",\n"
        << "\"mapDirectory\":" << jsonString(_mapDirectory) << ",\n\"map\":" << _mapId << ",\n";
    if (!_journal.Edits().empty())
    {
      char bounds[160];
      std::snprintf(bounds, sizeof(bounds), "\"bounds\":{\"min\":[%.3f,%.3f,%.3f],\"max\":[%.3f,%.3f,%.3f]},\n",
                    double(lo.x), double(lo.y), double(lo.z), double(hi.x), double(hi.y), double(hi.z));
      out << bounds;
    }
    // The ADTs the edits were seeded from: an import can refuse or warn when the server's differ.
    out << "\"seedTiles\":[";
    bool first = true;
    for (auto const& t : tiles)
    {
      std::vector<char> bytes;
      readAdt(t.first, t.second, bytes);
      QByteArray const md5 = bytes.empty()
        ? QByteArray()
        : QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(), int(bytes.size())), QCryptographicHash::Md5).toHex();
      out << (first ? "\n" : ",\n") << "{\"x\":" << t.first << ",\"y\":" << t.second << ",\"size\":" << bytes.size()
          << ",\"md5\":" << jsonString(md5.toStdString()) << "}";
      first = false;
    }
    out << "\n],\n\"strokes\":[";
    first = true;
    for (auto const& kv : strokes)
    {
      out << (first ? "\n" : ",\n") << "{\"stroke\":" << kv.first << ",\"edits\":" << kv.second.edits
          << ",\"firstSeq\":" << kv.second.firstSeq << ",\"lastSeq\":" << kv.second.lastSeq << "}";
      first = false;
    }
    out << "\n],\n\"edits\":[\n";
    first = true;
    for (Edit const& e : _journal.Edits())
    {
      out << (first ? "" : ",\n") << editLine(e);
      first = false;
    }
    out << "\n]\n}\n";

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
      return {};
    std::string const text = out.str();
    file.write(text.data(), qint64(text.size()));
    if (!file.commit())
      return {};
    return path.toStdString();
  }

  // ------------------------------------------------------------------------------------------ rendering
  void EverwoodLayer::prepareFrame()
  {
    if (_glContext != QOpenGLContext::currentContext())
    {
      forgetGl();
      _glContext = QOpenGLContext::currentContext();
    }

    for (auto& kv : _masks)
    {
      TileMask& mask = kv.second;
      if (mask.dirtyHi < 0 && mask.texture)
        continue;
      if (!mask.hidden && !mask.texture)
      {
        mask.dirtyHi = -1;
        continue;
      }

      GLint previousUnit = 0;
      gl.getIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
      gl.activeTexture(GL_TEXTURE0 + MASK_UNIT);
      if (!mask.texture)
      {
        gl.genTextures(1, &mask.texture);
        gl.bindTexture(GL_TEXTURE_2D, mask.texture);
        gl.texImage2D(GL_TEXTURE_2D, 0, GL_R8, MASK_SIZE, MASK_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, mask.pixels.data());
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
      }
      else
      {
        gl.bindTexture(GL_TEXTURE_2D, mask.texture);
        gl.texSubImage2D(GL_TEXTURE_2D, 0, 0, mask.dirtyLo, MASK_SIZE, mask.dirtyHi - mask.dirtyLo + 1
                         , GL_RED, GL_UNSIGNED_BYTE, mask.pixels.data() + size_t(mask.dirtyLo) * MASK_SIZE);
      }
      gl.activeTexture(static_cast<GLenum>(previousUnit));
      mask.dirtyHi = -1;
    }
  }

  void EverwoodLayer::bindTerrainMask(OpenGL::Scoped::use_program& shader, MapTile* tile)
  {
    auto it = _masks.find(int(tile->index.x) * 64 + int(tile->index.z));
    bool const on = it != _masks.end() && it->second.hidden > 0 && it->second.texture;
    shader.uniform("everwood_mask_on", on);
    if (!on)
      return;

    GLint previousUnit = 0;
    gl.getIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
    gl.activeTexture(GL_TEXTURE0 + MASK_UNIT);
    gl.bindTexture(GL_TEXTURE_2D, it->second.texture);
    gl.activeTexture(static_cast<GLenum>(previousUnit));
    shader.uniform("everwood_mask_origin", glm::vec2(it->second.origin));
  }

  bool EverwoodLayer::buildMesh(Key const& key, ChunkGl& out)
  {
    // Which lattice cells of the window lie near a changed column: only those triangles are drawn, the real
    // terrain is drawn everywhere else. Unchanged chunk column and neighbours: nothing to show.
    int32_t const bx = key[0] * C - 1, by = key[1] * C - 1;
    int const reach = 2;
    int const nearSize = SAMPLES + 1;
    std::vector<uint8_t> nearChanged(size_t(nearSize) * nearSize, 0);
    bool anyNear = false;
    for (int32_t ly = -reach; ly < SAMPLES + reach; ++ly)
      for (int32_t lx = -reach; lx < SAMPLES + reach; ++lx)
      {
        if (!columnChangedAt(bx + lx, by + ly))
          continue;
        for (int32_t cy = std::max(ly - reach, 0); cy <= std::min(ly + reach, nearSize - 1); ++cy)
          for (int32_t cx = std::max(lx - reach, 0); cx <= std::min(lx + reach, nearSize - 1); ++cx)
            nearChanged[size_t(cx) + size_t(cy) * nearSize] = 1;
        anyNear = true;
      }

    out.indexCount = 0;
    if (!anyNear)
    {
      out.keepAlive.clear();
      out.arrays.clear();
      out.sources.clear();
      return true;
    }

    std::vector<int8_t> window(WINDOW);
    _field.Window(key, window.data());
    Mesh mesh;
    SurfaceNets(window.data(), mesh);
    if (mesh.idx.empty())
    {
      out.keepAlive.clear();
      out.arrays.clear();
      out.sources.clear();
      return true;
    }

    std::vector<float> seedHeights(size_t(SAMPLES) * SAMPLES);
    _field.WindowSeedHeights(key, seedHeights.data());
    auto const origin = WindowOrigin(key);
    std::vector<float> edited, tilt;
    EditedWeights(mesh, window.data(), seedHeights.data(), origin[2], edited);
    SeedTiltCos(mesh, seedHeights.data(), tilt);

    // Every tile under the vertices must be in memory for its textures; come back later otherwise.
    size_t const nv = mesh.pos.size() / 3;
    struct ChunkLayers
    {
      MapChunk* chunk = nullptr;
      int tex[4] = { -1, -1, -1, -1 };
      int count = 0;
    };
    std::unordered_map<int, ChunkLayers> layersByChunk;
    std::vector<scoped_blp_texture_reference> keepAlive;
    std::vector<unsigned> arrays;
    std::vector<ChunkSource> sources;
    bool waiting = false;

    // The one ADT chunk that paints a piece of the mesh: its layers, alpha, shadow and vertex colour.
    auto layersOf = [&](int tileX, int tileZ, int cx, int cz) -> ChunkLayers const*
    {
      if (tileX < 0 || tileZ < 0 || tileX >= 64 || tileZ >= 64)
        return nullptr;
      int const id = (tileX * 64 + tileZ) * 256 + cz * 16 + cx;
      auto found = layersByChunk.find(id);
      if (found != layersByChunk.end())
        return &found->second;

      TileIndex const index(static_cast<std::size_t>(tileX), static_cast<std::size_t>(tileZ));
      ChunkLayers layers;
      if (_world->mapIndex.hasTile(index))
      {
        if (!_world->mapIndex.tileLoaded(index))
        {
          waiting = true;
          return nullptr;
        }
        MapTile* tile = _world->mapIndex.getTile(index);
        if (!tile || !tile->finishedLoading() || !tile->texturesFinishedLoading())
        {
          waiting = true;
          return nullptr;
        }
        MapChunk* chunk = tile->getChunk(static_cast<unsigned>(cx), static_cast<unsigned>(cz));
        if (chunk && chunk->texture_set)
        {
          layers.chunk = chunk;
          // Read before the textures: a stamp bumped while this runs is seen on the next frame.
          sources.push_back({ tileX, tileZ, cx, cz, chunk->appearanceStamp() });
          auto& textures = chunk->texture_set->renderTextures();
          layers.count = std::min<int>(4, int(chunk->texture_set->renderTextureCount()));
          for (int k = 0; k < layers.count; ++k)
          {
            textures[k]->upload();
            if (!textures[k]->is_uploaded())
            {
              waiting = true;
              return nullptr;
            }
            GLuint const array = textures[k]->texture_array();
            auto slot = std::find(arrays.begin(), arrays.end(), unsigned(array));
            if (slot == arrays.end())
            {
              if (arrays.size() >= size_t(NUM_TEXTURE_UNITS))
                continue;   // beyond the shader's samplers: the layer stays out of this mesh
              arrays.push_back(array);
              slot = arrays.end() - 1;
            }
            // The terrain drops a layer's specular mask with a negative array index, so -0 keeps it.
            bool const specular = textures[k]->is_specular() || textures[k]->array_index() == 0;
            layers.tex[k] = int(slot - arrays.begin()) * 4096 + textures[k]->array_index() + (specular ? LAYER_SPECULAR : 0);
            keepAlive.push_back(textures[k]);
          }
        }
      }
      return &layersByChunk.emplace(id, layers).first->second;
    };

    auto cellNear = [&](float wx, float wy) -> bool
    {
      int32_t const cx = int32_t(std::floor(wx / VOXEL_SIZE)) - bx, cy = int32_t(std::floor(wy / VOXEL_SIZE)) - by;
      if (cx < 0 || cy < 0 || cx >= nearSize || cy >= nearSize)
        return false;
      return nearChanged[size_t(cx) + size_t(cy) * nearSize] != 0;
    };

    // Texture repeats: Noggit's terrain uses 8 per chunk, along its own x and z, which is what a world-based
    // planar mapping reproduces. Keep the numbers small by starting from a whole number of repeats.
    float const unitSize = CHUNKSIZE / 8.f;
    glm::vec3 const originNoggit = wowToNoggit({ origin[0], origin[1], origin[2] });
    float const uvBaseU = std::floor(originNoggit.x / unitSize) * unitSize;
    float const uvBaseV = std::floor(originNoggit.z / unitSize) * unitSize;

    struct MeshVertex
    {
      glm::vec3 pos, normal;
      float wall;
    };
    auto mixVertex = [](MeshVertex const& a, MeshVertex const& b, float t) -> MeshVertex
    {
      return { glm::mix(a.pos, b.pos, t), glm::mix(a.normal, b.normal, t), a.wall + (b.wall - a.wall) * t };
    };

    std::vector<MeshVertex> shaped(nv);
    std::vector<uint8_t> vertexNear(nv, 0);
    for (size_t i = 0; i < nv; ++i)
    {
      float const wx = origin[0] + mesh.pos[i * 3], wy = origin[1] + mesh.pos[i * 3 + 1], wz = origin[2] + mesh.pos[i * 3 + 2];
      vertexNear[i] = cellNear(wx, wy) ? 1 : 0;
      glm::vec3 const g(mesh.nrm[i * 3], mesh.nrm[i * 3 + 1], mesh.nrm[i * 3 + 2]);
      float const gl_ = glm::length(g);
      glm::vec3 const nw = gl_ > 1e-6f ? g / gl_ : glm::vec3(0, 0, 1);

      // CONTRACT v3.3.6: an EDITED face keeps the ground's look while nearly level or within ~40 degrees of
      // the seeded ground's normal, and turns to the wall layer only where it faces away from it. A face
      // pointing down is always a wall.
      float wall = edited[i] * (1.f - smoothstep01(0.55f, 0.80f, tilt[i]));
      if (nw.z < -0.3f)
        wall = std::max(wall, edited[i]);

      // The normal in the terrain's own convention, which its lighting expects: MCNR order as MapChunk loads
      // it, WoW x, up, WoW y. Noggit's world axes (-WoW y, up, -WoW x) would light any slope wrongly.
      glm::vec3 const pos = wowToNoggit({ wx, wy, wz - RENDER_DROP });
      glm::vec3 normal(nw.x, nw.z, nw.y);

      // The ADT's stored normals are smoothed and differ from the seed's geometry by a degree or two. Carry that
      // difference over, so untouched ground is lit exactly as the terrain beside it and an edit only adds its
      // own change of slope. Walls have nothing to match.
      if (wall < 1.f)
      {
        int const gx = int(std::floor(pos.x / CHUNKSIZE)), gz = int(std::floor(pos.z / CHUNKSIZE));
        ChunkLayers const* layers = gx >= 0 && gz >= 0 ? layersOf(gx / 16, gz / 16, gx % 16, gz % 16) : nullptr;
        if (waiting)
          return false;
        float const fx = mesh.pos[i * 3] / VOXEL_SIZE, fy = mesh.pos[i * 3 + 1] / VOXEL_SIZE;
        int32_t const sx = std::clamp(int32_t(std::floor(fx)), 0, SAMPLES - 2), sy = std::clamp(int32_t(std::floor(fy)), 0, SAMPLES - 2);
        float const h00 = seedHeights[size_t(sx) + size_t(sy) * SAMPLES], h10 = seedHeights[size_t(sx + 1) + size_t(sy) * SAMPLES];
        float const h01 = seedHeights[size_t(sx) + size_t(sy + 1) * SAMPLES], h11 = seedHeights[size_t(sx + 1) + size_t(sy + 1) * SAMPLES];
        if (layers && layers->chunk && std::isfinite(h00) && std::isfinite(h10) && std::isfinite(h01) && std::isfinite(h11))
        {
          float const tx = std::clamp(fx - float(sx), 0.f, 1.f), ty = std::clamp(fy - float(sy), 0.f, 1.f);
          float const dhx = ((h10 - h00) * (1.f - ty) + (h11 - h01) * ty) / VOXEL_SIZE;
          float const dhy = ((h01 - h00) * (1.f - tx) + (h11 - h10) * tx) / VOXEL_SIZE;
          glm::vec3 const seedNormal = glm::normalize(glm::vec3(-dhx, 1.f, -dhy));

          MapChunk* chunk = layers->chunk;
          auto const& buffer = chunk->mt->getChunkHeightmapBuffer();
          size_t const start = size_t(chunk->px * 16 + chunk->py) * mapbufsize * 4;
          FanPoint const fan = fanPoint((pos.x - chunk->xbase) / CHUNKSIZE, (pos.z - chunk->zbase) / CHUNKSIZE);
          glm::vec3 stored(0.f);
          for (int k = 0; k < 3; ++k)
          {
            size_t const at = start + size_t(fan.index[k]) * 4;
            stored += glm::vec3(buffer[at], buffer[at + 1], buffer[at + 2]) * fan.weight[k];
          }
          glm::vec3 const delta = stored - seedNormal;
          // More than ~20 degrees: the ADT was sculpted since the seed, the difference means nothing until a Re-seed.
          if (glm::dot(delta, delta) < 0.12f)
            normal = glm::normalize(normal + delta * (1.f - wall));
        }
      }
      shaped[i] = { pos, normal, wall };
    }

    constexpr size_t STRIDE = 20;
    std::vector<float> vertices;
    vertices.reserve(mesh.idx.size() * STRIDE);
    glm::vec3 bmin(std::numeric_limits<float>::max()), bmax(std::numeric_limits<float>::lowest());

    auto addVertex = [&](MeshVertex const& m, ChunkLayers const* layers)
    {
      float v[STRIDE];
      v[0] = m.pos.x; v[1] = m.pos.y; v[2] = m.pos.z;
      v[3] = m.normal.x; v[4] = m.normal.y; v[5] = m.normal.z;
      v[6] = (m.pos.x - uvBaseU) / unitSize;
      v[7] = (m.pos.z - uvBaseV) / unitSize;
      v[8] = v[9] = v[10] = 0.f;
      v[11] = m.wall;
      v[12] = v[13] = v[14] = v[15] = -1.f;
      v[16] = v[17] = v[18] = 1.f;
      v[19] = 0.f;
      if (layers && layers->chunk)
      {
        for (int k = 0; k < 4; ++k)
          v[12 + k] = float(layers->tex[k]);
        MapChunk* chunk = layers->chunk;
        float const u = (m.pos.x - chunk->xbase) / CHUNKSIZE;
        float const w = (m.pos.z - chunk->zbase) / CHUNKSIZE;

        // Alpha and shadow maps: what GL_LINEAR + GL_CLAMP_TO_EDGE returns for the terrain, 64 x 64 per chunk,
        // row along z, column along x.
        float const fx = std::clamp(u * 64.f - 0.5f, 0.f, 63.f), fz = std::clamp(w * 64.f - 0.5f, 0.f, 63.f);
        int const x0 = int(fx), z0 = int(fz), x1 = std::min(x0 + 1, 63), z1 = std::min(z0 + 1, 63);
        float const tx = fx - float(x0), tz = fz - float(z0);
        size_t const o00 = size_t(z0) * 64 + size_t(x0), o10 = size_t(z0) * 64 + size_t(x1);
        size_t const o01 = size_t(z1) * 64 + size_t(x0), o11 = size_t(z1) * 64 + size_t(x1);
        auto bilinear = [&](auto const& read)
        {
          return (read(o00) * (1.f - tx) + read(o10) * tx) * (1.f - tz) + (read(o01) * (1.f - tx) + read(o11) * tx) * tz;
        };
        auto* textureSet = chunk->texture_set.get();
        auto& temp = textureSet->getTempAlphamaps();
        auto* alphamaps = textureSet->getAlphamaps();
        for (int a = 0; a + 1 < layers->count && a < 3; ++a)
        {
          if (temp)
            v[8 + a] = bilinear([&](size_t o) { return (*temp)[a + 1][o] / 255.f; });
          else if ((*alphamaps)[a])
            v[8 + a] = bilinear([&](size_t o) { return float((*alphamaps)[a]->getAlpha(o)) / 255.f; });
        }
        v[19] = bilinear([&](size_t o) { return float(chunk->_shadow_map[o]) / 255.f; });

        // Vertex colours: the terrain's own triangle fan around each cell's centre vertex.
        FanPoint const fan = fanPoint(u, w);
        glm::vec3 color(0.f);
        for (int k = 0; k < 3; ++k)
          color += chunk->mccv[fan.index[k]] * fan.weight[k];
        v[16] = color.r; v[17] = color.g; v[18] = color.b;
      }
      vertices.insert(vertices.end(), v, v + STRIDE);
      bmin = glm::min(bmin, m.pos);
      bmax = glm::max(bmax, m.pos);
    };

    // A triangle wears one chunk: layer sets are per chunk and in a chunk's own order, so a triangle across a
    // chunk line is cut along it, and the seam falls exactly where the terrain's does.
    std::vector<MeshVertex> poly, clipped;
    auto clipAxis = [&](bool xAxis, float limit, bool keepBelow)
    {
      clipped.clear();
      auto inside = [&](MeshVertex const& m)
      {
        float const c = xAxis ? m.pos.x : m.pos.z;
        return keepBelow ? c <= limit : c >= limit;
      };
      for (size_t i = 0; i < poly.size(); ++i)
      {
        MeshVertex const& a = poly[i];
        MeshVertex const& b = poly[(i + 1) % poly.size()];
        bool const ia = inside(a), ib = inside(b);
        if (ia)
          clipped.push_back(a);
        if (ia != ib)
        {
          float const ca = xAxis ? a.pos.x : a.pos.z, cb = xAxis ? b.pos.x : b.pos.z;
          clipped.push_back(mixVertex(a, b, (limit - ca) / (cb - ca)));
        }
      }
      poly.swap(clipped);
    };

    for (size_t t = 0; t + 2 < mesh.idx.size(); t += 3)
    {
      uint32_t const i0 = mesh.idx[t], i1 = mesh.idx[t + 1], i2 = mesh.idx[t + 2];
      if (!vertexNear[i0] && !vertexNear[i1] && !vertexNear[i2])
        continue;
      MeshVertex const& a = shaped[i0];
      MeshVertex const& b = shaped[i1];
      MeshVertex const& c = shaped[i2];
      int const gx0 = int(std::floor(std::min({ a.pos.x, b.pos.x, c.pos.x }) / CHUNKSIZE));
      int const gx1 = int(std::floor(std::max({ a.pos.x, b.pos.x, c.pos.x }) / CHUNKSIZE));
      int const gz0 = int(std::floor(std::min({ a.pos.z, b.pos.z, c.pos.z }) / CHUNKSIZE));
      int const gz1 = int(std::floor(std::max({ a.pos.z, b.pos.z, c.pos.z }) / CHUNKSIZE));
      for (int gz = gz0; gz <= gz1; ++gz)
        for (int gx = gx0; gx <= gx1; ++gx)
        {
          ChunkLayers const* layers = layersOf(gx >= 0 ? gx / 16 : -1, gz >= 0 ? gz / 16 : -1, gx >= 0 ? gx % 16 : 0, gz >= 0 ? gz % 16 : 0);
          if (waiting)
            return false;
          if (gx0 == gx1 && gz0 == gz1)
          {
            addVertex(a, layers);
            addVertex(b, layers);
            addVertex(c, layers);
            continue;
          }
          poly = { a, b, c };
          clipAxis(true, float(gx) * CHUNKSIZE, false);
          clipAxis(true, float(gx + 1) * CHUNKSIZE, true);
          clipAxis(false, float(gz) * CHUNKSIZE, false);
          clipAxis(false, float(gz + 1) * CHUNKSIZE, true);
          for (size_t k = 1; k + 1 < poly.size(); ++k)
          {
            addVertex(poly[0], layers);
            addVertex(poly[k], layers);
            addVertex(poly[k + 1], layers);
          }
        }
    }

    out.boundsMin = bmin;
    out.boundsMax = bmax;
    out.indexCount = int(vertices.size() / STRIDE);
    out.arrays = std::move(arrays);
    out.keepAlive = std::move(keepAlive);
    out.sources = std::move(sources);

    if (!out.vao)
    {
      gl.genVertexArrays(1, &out.vao);
      gl.genBuffers(1, &out.vbo);
    }
    gl.bindVertexArray(out.vao);
    gl.bindBuffer(GL_ARRAY_BUFFER, out.vbo);
    gl.bufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(float)), vertices.data(), GL_DYNAMIC_DRAW);

    GLsizei const stride = GLsizei(STRIDE * sizeof(float));
    // The locations are fixed in everwood_voxel_vert.glsl.
    auto attrib = [&](GLuint location, GLint size, size_t offset)
    {
      gl.enableVertexAttribArray(location);
      gl.vertexAttribPointer(location, size, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offset * sizeof(float)));
    };
    attrib(0, 3, 0);    // position
    attrib(1, 3, 3);    // normal
    attrib(2, 2, 6);    // texcoord
    attrib(3, 3, 8);    // alpha
    attrib(4, 1, 11);   // wall
    attrib(5, 4, 12);   // layers
    attrib(6, 3, 16);   // vertex colour
    attrib(7, 1, 19);   // baked shadow
    gl.bindVertexArray(0);
    gl.bindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
  }

  void EverwoodLayer::draw(glm::mat4x4 const& mvp, glm::vec3 const& cameraPos, VoxelCursor const& cursor)
  {
    if (_journal.Edits().empty() && _meshes.empty())
      return;

    if (!_program)
    {
      _program.reset(new OpenGL::program
        { { GL_VERTEX_SHADER, OpenGL::shader::src_from_qrc("everwood_voxel_vs") }
        , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("everwood_voxel_fs") }
        });
      OpenGL::Scoped::use_program shader{ *_program };
      shader.bind_uniform_block("matrices", 0);
      shader.bind_uniform_block("lighting", 1);
      shader.bind_uniform_block("overlay_params", 2);
      shader.uniform("stamp_brush", 4);   // the unit the terrain's stamp cursor uses
      std::vector<int> samplers;
      for (int i = 0; i < NUM_TEXTURE_UNITS; ++i)
        samplers.push_back(FIRST_TEXTURE_UNIT + i);
      shader.uniform("textures", samplers);
    }

    // Terrain painted, or otherwise recoloured, since the mesh was built: rebuild it from the chunks as they
    // are now. Nothing here touches the density.
    for (auto const& kv : _meshes)
      for (ChunkSource const& source : kv.second.sources)
      {
        TileIndex const index(static_cast<std::size_t>(source.tileX), static_cast<std::size_t>(source.tileZ));
        if (!_world->mapIndex.tileLoaded(index))
          continue;
        MapChunk* chunk = _world->mapIndex.getTile(index)->getChunk(static_cast<unsigned>(source.cx), static_cast<unsigned>(source.cz));
        if (chunk && chunk->appearanceStamp() != source.stamp)
        {
          _dirtyMeshes.insert(kv.first);
          break;
        }
      }

    // Everything edited since the last frame, and whatever waited for its tile.
    for (auto it = _dirtyMeshes.begin(); it != _dirtyMeshes.end();)
    {
      ChunkGl& mesh = _meshes[*it];
      if (buildMesh(*it, mesh))
        it = _dirtyMeshes.erase(it);
      else
        ++it;
    }

    math::frustum const frustum(mvp);
    GLint previousUnit = 0;
    gl.getIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
    GLboolean const cullWasOn = gl.isEnabled(GL_CULL_FACE);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_BLEND);
    gl.enable(GL_DEPTH_TEST);

    {
      OpenGL::Scoped::use_program shader{ *_program };
      shader.uniform("camera", cameraPos);
      shader.uniform("draw_cursor_circle", cursor.type);
      if (cursor.type != 0)
      {
        shader.uniform("cursor_position", cursor.position);
        shader.uniform("cursorRotation", cursor.rotation);
        shader.uniform("outer_cursor_radius", cursor.radius);
        shader.uniform("inner_cursor_ratio", cursor.innerRatio);
        shader.uniform("cursor_color", cursor.color);
      }
      for (auto const& kv : _meshes)
      {
        ChunkGl const& mesh = kv.second;
        if (!mesh.indexCount || !frustum.intersects(mesh.boundsMax, mesh.boundsMin))
          continue;
        for (int i = 0; i < NUM_TEXTURE_UNITS; ++i)
        {
          gl.activeTexture(GL_TEXTURE0 + FIRST_TEXTURE_UNIT + i);
          gl.bindTexture(GL_TEXTURE_2D_ARRAY, i < int(mesh.arrays.size()) ? mesh.arrays[i] : 0);
        }
        gl.bindVertexArray(mesh.vao);
        gl.drawArraysInstanced(GL_TRIANGLES, 0, mesh.indexCount, 1);
      }
    }

    gl.bindVertexArray(0);
    gl.activeTexture(static_cast<GLenum>(previousUnit));
    if (cullWasOn)
      gl.enable(GL_CULL_FACE);
  }

  void EverwoodLayer::freeChunkGl(ChunkGl& mesh)
  {
    if (mesh.vao)
    {
      gl.deleteVertexArray(1, &mesh.vao);
      gl.deleteBuffers(1, &mesh.vbo);
    }
    mesh.vao = mesh.vbo = 0;
    mesh.indexCount = 0;
  }

  void EverwoodLayer::forgetGl()
  {
    // The context changed: the old handles are gone with it, every mesh and mask is uploaded again.
    for (auto& kv : _meshes)
    {
      kv.second.vao = kv.second.vbo = 0;
      kv.second.indexCount = 0;
      _dirtyMeshes.insert(kv.first);
    }
    for (auto& kv : _masks)
    {
      kv.second.texture = 0;
      kv.second.dirtyLo = 0;
      kv.second.dirtyHi = MASK_SIZE - 1;
    }
    _program.release();
  }

  void EverwoodLayer::releaseGl()
  {
    for (auto& kv : _meshes)
      freeChunkGl(kv.second);
    _meshes.clear();
    for (auto& kv : _masks)
      if (kv.second.texture)
      {
        gl.deleteTextures(1, &kv.second.texture);
        kv.second.texture = 0;
      }
    _program.reset();
  }
}
