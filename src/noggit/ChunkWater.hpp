// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <math/frustum.hpp>
#include <noggit/liquid_layer.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/tool_enums.hpp>

#include <cstdint>
#include <optional>
#include <vector>

class MapChunk;
class TileWater;

// Which MH2O per-cell attribute mask an attribute-painting operation targets (Azure port).
enum class LiquidAttribute
{
  Fishable,
  Fatigue
};

// Snapshot of a chunk's liquid for undo/redo (Azure port). liquid_layer is copyable.
struct ChunkWaterState
{
  std::vector<liquid_layer> layers;
  MH2O_Attributes attributes;
  bool auto_update_attributes = true;
  // Everwood water flow (EVERWOOD-WATER-FLOW.md), per chunk; carried through undo/redo.
  std::uint32_t n_flowvs = 0;
  mclq_flowvs flowvs[2] = {};
};

namespace BlizzardArchive
{
  class ClientFile;
}

namespace util
{
  class sExtendableArray;
}

class ChunkWater
{
public:
  ChunkWater() = delete;
  explicit ChunkWater(MapChunk* chunk, TileWater* water_tile, float x, float z, bool use_mclq_green_lava);

  ChunkWater (ChunkWater const&) = delete;
  ChunkWater (ChunkWater&&) = delete;
  ChunkWater& operator= (ChunkWater const&) = delete;
  ChunkWater& operator= (ChunkWater&&) = delete;

  void from_mclq(std::vector<mclq>& layers);
  void fromFile(BlizzardArchive::ClientFile& f, size_t basePos);
  void save(util::sExtendableArray& adt, int base_pos, int& header_pos, int& current_pos);
  // MUST be called before sizing/writing an mclq-mode MCNK: drops empty layers and refreshes
  // attributes so layer_count() matches what save_mclq will actually write. When cleanup lived
  // inside save_mclq, the caller sized the chunk with the PRE-cleanup count and every tile
  // where a layer got dropped saved corrupt (declared size != written bytes).
  void prepare_mclq_save();
  void save_mclq(util::sExtendableArray& adt, int mcnk_pos, int& current_pos);

  bool is_visible ( const float& cull_distance
                  , const math::frustum& frustum
                  , const glm::vec3& camera
                  , display_mode display
                  ) const;

  void autoGen(MapChunk* chunk, float factor);
  void update_underground_vertices_depth(MapChunk* chunk);
  void CropWater(MapChunk* chunkTerrain);

  void setType(int type, size_t layer);
  int getType(size_t layer) const;
  bool hasData(size_t layer) const;
  void tagUpdate();

  std::vector<liquid_layer>* getLayers();
  // Viewport-only liquid layers used by Chunk Mover previews. They are kept
  // separate from _layers so previewing can never dirty or serialize an ADT.
  // getRenderLayers returns the preview layers when present, otherwise _layers.
  // (Ported from Noggit Azure; the MCLQ save path still reads _layers only.)
  std::vector<liquid_layer>* getRenderLayers();
  void setChunkMoverLiquidPreview(std::optional<std::vector<liquid_layer>> layers);

  // update every layer's render
  void update_layers();
  float getMinHeight() const;
  float getMaxHeight() const;

  void paintLiquid( glm::vec3 const& pos
                  , float radius
                  , int liquid_id
                  , bool add
                  , math::radians const& angle
                  , math::radians const& orientation
                  , bool lock
                  , glm::vec3 const& origin
                  , bool override_height
                  , bool override_liquid_id
                  , MapChunk* chunk
                  , float opacity_factor
                  , int target_layer = -1
                  , std::uint64_t surface_token = 0
                  );

  // Stroke-based depth/opacity and world-aligned UV projection on one surface (Azure port).
  void paintDepth(glm::vec3 const& pos, float radius, float depth, int target_layer,
                  std::uint64_t surface_token = 0);
  void projectUV(glm::vec3 const& pos, float radius, float scale, math::radians rotation,
                 int target_layer, std::uint64_t surface_token = 0);

  MapChunk* getChunk();
  TileWater* getWaterTile();

  MH2O_Attributes const& getAttributes() const;
  MH2O_Attributes& getAttributes();

  // Undo/redo snapshot of the whole liquid state (Azure port).
  ChunkWaterState getState() const;
  void restoreState(ChunkWaterState const& state);

  // Per-cell attribute painting / clearing (Azure port). Each returns true when the
  // chunk's masks changed so the caller can skip no-op tiles. NOTE: these only edit
  // the in-memory masks and stop update_layers() from re-deriving them; the frozen
  // MCLQ/MH2O save paths still regenerate attributes, so persisting authored masks to
  // disk needs a save-path gate -- see paintAttribute in ChunkWater.cpp.
  bool paintAttribute(glm::vec3 const& pos, float radius, LiquidAttribute attribute, bool value,
                      int target_layer = -1, std::uint64_t surface_token = 0);
  bool clearAttribute(LiquidAttribute attribute);
  bool clearAttributes();
  bool clearFishableAttributesOutsideLiquid();
  bool regenerateAttributes();

  // Everwood water flow (EVERWOOD-WATER-FLOW.md): per chunk, written into every MCLQ layer of the
  // chunk on save. Positions / directions are WoW world coordinates. MH2O is not touched.
  std::uint32_t flowCount() const { return _n_flowvs; }
  mclq_flowvs const& flowSlot(int i) const { return _flowvs[i]; }
  void setFlow(mclq_flowvs const& slot0);  // one flow point (n_flowvs = 1)
  void clearFlow();                        // n_flowvs = 0, slots zeroed

  float xbase, zbase;

  int layer_count() const;

private:
  MH2O_Attributes attributes;

  glm::vec3 vmin, vmax, vcenter;
  bool _use_mclq_green_lava;

  // remove empty layers
  void cleanup();

  void copy_height_to_layer(liquid_layer& target, glm::vec3 const& pos, float radius);
  // Map an explicit surface selection to a concrete layer index (Azure port):
  // prefers the surface_token match, else falls back to target_layer.
  int resolve_layer(int target_layer, std::uint64_t surface_token) const;
  // Union of every layer's covered-subchunk mask (Azure port).
  std::uint64_t liquidCellMask() const;

  std::uint32_t _n_flowvs = 0;
  mclq_flowvs _flowvs[2] = {};

  bool _auto_update_attributes = true;
  // updates attributes for all layers
  void update_attributes();

  std::vector<liquid_layer> _layers;
  std::optional<std::vector<liquid_layer>> _chunk_mover_preview_layers;
  int _layer_count = 0;

  MapChunk* _chunk;
  TileWater* _water_tile;

  friend class MapView;
};
