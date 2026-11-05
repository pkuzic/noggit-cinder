// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// One map's Everwood voxel layer in the editor: the edit journal, the density world seeded from the raw
// ADT files, undo by canonical rebuild, the .mvoxj file and the export, and the rendering (voxel mesh over
// the terrain, ADT terrain hidden where the field was edited). Exists only in a project with EverwoodMode.
// See EVERWOOD-MODE.md.
#pragma once

#include <noggit/everwood/AdtTerrain.hpp>
#include <noggit/everwood/VoxelField.hpp>
#include <noggit/TextureManager.h>
#include <opengl/types.hpp>

#include <glm/glm.hpp>

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

  class EverwoodLayer
  {
  public:
    EverwoodLayer(::World* world, std::string projectPath, std::string mapDirectory, uint16_t mapId);
    ~EverwoodLayer();

    EverwoodLayer(EverwoodLayer const&) = delete;
    EverwoodLayer& operator=(EverwoodLayer const&) = delete;

    // ---- edits
    // A new stroke id (1..65535, wraps). Strokes that were undone can no longer be redone (CONTRACT 9.4).
    uint16_t beginStroke();
    // Stamps seq, nonce, stroke and map, journals and applies. Invalid edits are dropped.
    void addEdit(Voxel::Edit edit, uint16_t stroke);
    void endStroke();

    // Undo (true) / redo (false) one stroke: the canonical rebuild of its read closure.
    void setStrokeUndone(uint16_t stroke, bool undone);

    // ---- queries
    bool rawGround(float wowX, float wowY, float& height);
    // world.rs raymarch through the field (active chunk) or the raw ADT ground + lift (elsewhere).
    std::optional<AimHit> raycast(glm::vec3 const& wowOrigin, glm::vec3 const& wowDir, float maxDist);

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
    // Writes the documented export file; returns its path, empty on failure.
    std::string exportEdits(std::string const& projectName);
    // Forget the cached ADT heights and replay everything (the project's ADTs changed).
    void reseed();

    // ---- rendering, called by WorldRender with a current context
    void prepareFrame();
    void bindTerrainMask(OpenGL::Scoped::use_program& shader, MapTile* tile);
    void draw(glm::mat4x4 const& mvp, glm::vec3 const& cameraPos, bool drawFogAndLight = true);
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

    struct ChunkGl
    {
      unsigned vao = 0;
      unsigned vbo = 0;
      unsigned ibo = 0;
      int indexCount = 0;
      glm::vec3 boundsMin{ 0.f };
      glm::vec3 boundsMax{ 0.f };
      std::vector<unsigned> arrays;      // texture arrays by sampler slot
      std::vector<scoped_blp_texture_reference> keepAlive;
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

    void rebuildJournal();
    void noteChanged(std::vector<Voxel::Key> const& keys, std::vector<Voxel::Key> const* added);
    void flushColumns();
    void recomputeColumn(Voxel::ColKey const& col);
    void writeMaskAround(Voxel::ColKey const& col);
    bool columnChangedAt(int32_t ix, int32_t iy) const;
    bool surfaceOkAt(int32_t ix, int32_t iy) const;
    TileMask& maskFor(int tileX, int tileY);
    void setMaskCell(int32_t i, int32_t j, bool hidden);

    bool buildMesh(Voxel::Key const& key, ChunkGl& gl);
    void forgetGl();
    void freeChunkGl(ChunkGl& gl);

    float aimDensity(glm::vec3 const& p);

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

    std::unordered_map<int, std::shared_ptr<Adt::Tile>> _tiles;   // tileX * 64 + tileY, null = no file
    std::unordered_map<Voxel::ColKey, ColumnState, Voxel::ColHash> _columns;
    std::unordered_set<Voxel::ColKey, Voxel::ColHash> _dirtyColumns;
    std::unordered_set<Voxel::Key, Voxel::KeyHash> _dirtyMeshes;
    std::unordered_map<Voxel::Key, ChunkGl, Voxel::KeyHash> _meshes;
    std::unordered_map<int, TileMask> _masks;

    std::unique_ptr<OpenGL::program> _program;
    QOpenGLContext* _glContext = nullptr;
  };
}
