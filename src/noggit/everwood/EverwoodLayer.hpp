// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// One map's Everwood voxel layer in the editor: the edit journal, the density world seeded from the raw
// ADT files, undo by canonical rebuild, the .mvoxj file and the export, and the rendering (voxel mesh over
// the terrain, ADT terrain hidden where the field was edited). Exists only in a project with EverwoodMode.
// See EVERWOOD-MODE.md.
#pragma once
#include <cstring>

#include <noggit/everwood/AdtTerrain.hpp>
#include <noggit/everwood/VoxelField.hpp>
#include <noggit/everwood/VoxelFloaters.hpp>
#include <noggit/TextureManager.h>
#include <opengl/types.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class MapTile;
class QOpenGLContext;
class World;

namespace OpenGL
{
  struct program;
  namespace Scoped
  {
    struct use_program;
  }
}

namespace Noggit::Everwood
{
  // Noggit's world axes <-> WoW's. Noggit x = ZEROPOINT - WoW y, Noggit y = WoW z, Noggit z = ZEROPOINT - WoW x.
  glm::vec3 wowToNoggit(glm::vec3 const& wow);
  glm::vec3 noggitToWow(glm::vec3 const& noggit);

  struct AimHit
  {
    glm::vec3 pos;      // WoW axes
    glm::vec3 normal;   // WoW axes, unit length
  };

  // The brush circle the terrain shader draws on the ground, for the part of the ground the mesh replaced.
  struct VoxelCursor
  {
    int type = 0;             // CursorType: 0 none
    glm::vec3 position{ 0.f };
    float rotation = 0.f;
    float radius = 0.f;
    float innerRatio = 0.f;
    glm::vec4 color{ 1.f };
  };

  // What the last journal load cost (Everwood panel "Map edits" line, smoke `journal`).
  struct JournalStats
  {
    bool loaded = false;        // a journal file was read
    size_t edits = 0;           // live edits it held
    uint64_t bytes = 0;         // its size on disk
    double readMs = 0;          // file read + JSON parse + wire decode
    double replayMs = 0;        // every edit applied to the field + the carve masks (mesh builds come later, per frame)
  };

  class EverwoodLayer
  {
  public:
    EverwoodLayer(::World* world, std::string projectPath, std::string mapDirectory, uint16_t mapId);
    ~EverwoodLayer();

    EverwoodLayer(EverwoodLayer const&) = delete;
    EverwoodLayer& operator=(EverwoodLayer const&) = delete;

    // ---- edits
    // A new stroke id (1..65535, wraps), never one that a loaded, live or undone edit still carries.
    uint16_t beginStroke();
    // The undone strokes can no longer be redone (CONTRACT 9.4). Call when a stroke opens its undo step.
    void dropUndone();
    // Stamps seq, nonce, stroke and map, journals and applies. Invalid edits are dropped.
    void addEdit(Voxel::Edit edit, uint16_t stroke);
    void endStroke();
    void beginBatch();
    void endBatch();

    // Undo (true) / redo (false) one stroke: the canonical rebuild of its read closure.
    void setStrokeUndone(uint16_t stroke, bool undone);

    // ---- queries
    bool rawGround(float wowX, float wowY, float& height);
    // world.rs raymarch through the field (active chunk) or the raw ADT ground + lift (elsewhere).
    std::optional<AimHit> raycast(glm::vec3 const& wowOrigin, glm::vec3 const& wowDir, float maxDist);
    // Where a terrain tool's cursor belongs, Noggit axes: the voxel surface when the ray meets it around an edit
    // before any visible terrain. terrainHit is the ray's ADT hit, which may be under or over the mesh.
    std::optional<glm::vec3> surfaceUnderCursor(glm::vec3 const& origin, glm::vec3 const& dir
                                               , std::optional<glm::vec3> const& terrainHit);

    // CONTRACT v3.10 3D paint (mode 7): how edited the voxel surface is at a hit (world.rs edited_weights, 0 on
    // the seeded ground, 1 far off it; 0 where no chunk is active), and the chunks that would refuse a dab
    // because they already hold 4 other cave textures (paint3.rs would_refuse3, on the live field).
    float editedWeightAt(AimHit const& hit);
    std::vector<Voxel::Key> refusingChunks3(Voxel::Edit const& e);
    size_t paintedChunkCount() const;
    uint64_t paintHash() const;

    // Remove floaters (VoxelFloaters.hpp): the plan only reads the field; its digs are ordinary edits the caller
    // sends through addEdit (one stroke = one undo step).
    Voxel::FloaterPlan planFloaters(uint32_t maxSize, Voxel::FloaterRegion const& region)
    {
      return Voxel::PlanFloaterRemoval(_field, maxSize, region);
    }
    std::vector<Voxel::Floater> findFloaters(uint32_t maxSize, Voxel::FloaterRegion const& region)
    {
      return Voxel::FindFloaters(_field, maxSize, region);
    }
    Voxel::World& field() { return _field; }          // test hook: density samples

    size_t liveEditCount() const { return _journal.Edits().size(); }
    Voxel::Edit const* lastEdit() const { return _journal.Edits().empty() ? nullptr : &_journal.Edits().back(); }
    size_t strokeCount() const;
    size_t activeChunkCount() const { return _field.ChunksRef().size(); }
    size_t hiddenCellCount() const;
    // FNV over the chunks whose density differs from the seed (sorted by key): the same field gives the
    // same hash however many untouched chunks happen to be active.
    uint64_t stateHash();
    std::string filePath() const;

    // ---- storage
    bool save();
    bool load();
    JournalStats const& loadStats() const { return _loadStats; }
    // The live journal's size as the .mvoxj file holds it (exact after save / load, else computed).
    uint64_t journalBytes();
    // Writes the documented export file; returns its path, empty on failure.
    std::string exportEdits(std::string const& projectName);
    // Forget the cached ADT heights and replay everything (the project's ADTs changed).
    void reseed();
    // Called every frame: once the heights of a tile the seed read have changed in this session (Noggit's terrain
    // tools, their undo, holes) and then stayed put for HEIGHT_SETTLE_MS, reseed from the live heights. Returns
    // true when it reseeded.
    bool followTerrainEdits();
    // Test hook: the largest |height| difference between the live seed of a loaded tile and the seed read from its
    // ADT bytes on disk, over a 0.5-yd grid. samples = points both answered (0 = not loaded or no file).
    float liveSeedDifference(int tileX, int tileY, size_t& samples);
    size_t liveSeedTileCount() const { return _liveTiles; }

    // ---- rendering, called by WorldRender with a current context
    void prepareFrame();
    void bindTerrainMask(OpenGL::Scoped::use_program& shader, MapTile* tile);
    void draw(glm::mat4x4 const& mvp, glm::vec3 const& cameraPos, VoxelCursor const& cursor);
    void releaseGl();

  private:
    struct Entry
    {
      Voxel::Edit edit;
      bool undone = false;
    };

    struct ColumnState
    {
      std::vector<uint8_t> changed;      // C^2, a sample of this lattice column differs from its seed
      bool any = false;
      bool surfaceOk = false;            // every surface chunk of the column is active
    };

    // An ADT chunk a mesh took its layers from, and the chunk's MapChunk::appearanceStamp() at that moment.
    struct ChunkSource
    {
      int tileX, tileZ, cx, cz;
      std::uint32_t stamp;
    };

    // Everything a mesh is computed from besides the ADT textures (those are tracked by the chunk stamps): the
    // mesh window, the seed heights under it, the lattice cells near an edited column and the paint samples
    // of the 27 chunks the window reads. A soft rebuild whose inputs equal the last build's gives the same mesh,
    // so it is skipped.
    struct MeshInputs
    {
      bool valid = false;
      std::vector<int8_t> window;
      std::vector<float> seed;
      std::vector<uint8_t> nearChanged;
      std::vector<uint16_t> paintMats;   // the 27 chunks' palettes, 0xFFFF between
      std::vector<uint32_t> paintRgba;   // the window's paint samples; empty when none of the 27 is painted
    };

    // What a mesh keeps of its inputs: two independent 64-bit digests (FNV-1a and a multiply-xorshift mix over
    // every input byte, lengths included) instead of the ~45-200 KB of inputs themselves.
    struct MeshDigest
    {
      bool valid = false;
      std::uint64_t a = 0, b = 0;
      bool operator==(MeshDigest const& o) const { return valid && o.valid && a == o.a && b == o.b; }
      static MeshDigest Of(MeshInputs const& in)
      {
        std::uint64_t fnv = 0xcbf29ce484222325ull, mix = 0x9e3779b97f4a7c15ull;
        // 8 bytes per step (the per-byte loop cost ~1.7 ms per skipped mesh check on painted chunks).
        auto word = [&](std::uint64_t w)
        {
          fnv = (fnv ^ w) * 0x100000001b3ull;
          fnv ^= fnv >> 32;
          mix = (mix ^ w) * 0xff51afd7ed558ccdull;
          mix ^= mix >> 29;
        };
        auto feed = [&](void const* p, size_t n)
        {
          unsigned char const* c = static_cast<unsigned char const*>(p);
          size_t i = 0;
          for (; i + 8 <= n; i += 8)
          {
            std::uint64_t w;
            std::memcpy(&w, c + i, 8);
            word(w);
          }
          std::uint64_t tail = 0;
          if (i < n)
            std::memcpy(&tail, c + i, n - i);
          word(tail);
          word(std::uint64_t(n));
        };
        feed(in.window.data(), in.window.size());
        feed(in.seed.data(), in.seed.size() * sizeof(float));
        feed(in.nearChanged.data(), in.nearChanged.size());
        feed(in.paintMats.data(), in.paintMats.size() * sizeof(std::uint16_t));
        feed(in.paintRgba.data(), in.paintRgba.size() * sizeof(std::uint32_t));
        MeshDigest d;
        d.valid = in.valid;
        d.a = fnv;
        d.b = mix;
        return d;
      }
    };

    struct ChunkGl
    {
      MeshDigest inputs;
      unsigned vao = 0;
      unsigned vbo = 0;
      int indexCount = 0;                // vertices of the triangle list
      glm::vec3 boundsMin{ 0.f };
      glm::vec3 boundsMax{ 0.f };
      std::vector<unsigned> arrays;      // texture arrays by sampler slot
      std::vector<scoped_blp_texture_reference> keepAlive;
      std::vector<ChunkSource> sources;
    };

    struct TileMask
    {
      std::vector<uint8_t> pixels;
      glm::ivec2 origin{ 0 };            // lattice index of texel (0, 0): WoW x, WoW y
      unsigned texture = 0;
      int dirtyLo = 0, dirtyHi = -1;     // dirty row range
      size_t hidden = 0;
    };

    bool seedHeight(float x, float y, float& h);
    Adt::Tile const* tileAt(int tileX, int tileY);
    void readAdt(int tileX, int tileY, std::vector<char>& out);
    // The tile's terrain as MapChunk::save would write it (same floats the game reads after a save); null when the
    // tile is not loaded. heightStamp = sum of its chunks' MapChunk::heightStamp() (0 = untouched this session).
    std::shared_ptr<Adt::Tile> liveTile(int tileX, int tileY, uint64_t* heightStamp);
    uint64_t tileHeightStamp(int tileX, int tileY) const;

    void rebuildJournal();
    std::string journalText() const;
    void noteChanged(std::vector<Voxel::Key> const& keys, std::vector<Voxel::Key> const* added);
    // 3D paint changed in these chunks: rebuild the meshes that read them (own + the window apron).
    void notePaintChanged(std::vector<Voxel::Key> const& keys);
    void flushColumns();
    void recomputeColumn(Voxel::ColKey const& col);
    void writeMaskAround(Voxel::ColKey const& col);
    bool columnChangedAt(int32_t ix, int32_t iy) const;
    bool surfaceOkAt(int32_t ix, int32_t iy) const;
    TileMask& maskFor(int tileX, int tileY);
    void setMaskCell(int32_t i, int32_t j, bool hidden);

    bool buildMesh(Voxel::Key const& key, ChunkGl& gl, bool soft = false);
    void forgetGl();
    void freeChunkGl(ChunkGl& gl);

    float aimDensity(glm::vec3 const& p);
    // Steps from t (unit direction, WoW axes) to the first solid; on a hit t is the hit's distance.
    std::optional<AimHit> march(glm::vec3 const& origin, glm::vec3 const& dir, float& t, float tEnd);
    bool cellHiddenAt(float wowX, float wowY) const;

    ::World* _world;
    std::string _projectPath;
    std::string _mapDirectory;
    uint16_t _mapId;

    std::vector<Entry> _entries;           // everything, including undone strokes (until a new stroke)
    Voxel::Journal _journal;               // the live edits in canonical order
    Voxel::World _field;
    uint32_t _nextSeq = 1;
    uint16_t _nextNonce = 1;
    uint16_t _nextStroke = 1;
    int _batchDepth = 0;
    JournalStats _loadStats;
    uint64_t _bytesCache = 0;
    bool _bytesValid = false;

    std::unordered_map<int, std::shared_ptr<Adt::Tile>> _tiles;   // tileX * 64 + tileY, null = no file
    std::unordered_map<int, uint64_t> _tileStamps;                 // the height stamp each cached tile was built at
    size_t _liveTiles = 0;                                         // cached tiles built from live heights
    bool _heightsPending = false;
    uint64_t _heightsPendingSig = 0;
    std::chrono::steady_clock::time_point _heightsPendingSince;
    std::unordered_map<Voxel::ColKey, ColumnState, Voxel::ColHash> _columns;
    std::unordered_set<Voxel::ColKey, Voxel::ColHash> _dirtyColumns;
    std::unordered_set<Voxel::Key, Voxel::KeyHash> _dirtyMeshes;   // rebuilt unconditionally (textures, new GL context)
    std::unordered_set<Voxel::Key, Voxel::KeyHash> _softMeshes;    // edit-driven: rebuilt only when their inputs changed
    std::unordered_map<Voxel::Key, ChunkGl, Voxel::KeyHash> _meshes;
    std::unordered_map<int, TileMask> _masks;
    glm::vec3 _editedMin{ 1.f }, _editedMax{ 0.f };   // WoW axes; empty when max < min

    // CONTRACT v3.10: the palette textures the 3D paint uses, by palette id (loaded once, shared by the meshes).
    std::unordered_map<uint16_t, scoped_blp_texture_reference> _paletteTextures;
    bool _paintBudgetLogged = false;

    std::unique_ptr<OpenGL::program> _program;
    QOpenGLContext* _glContext = nullptr;
  };
}
