// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ChunkWater.hpp>
#include <noggit/TileWater.hpp>
#include <noggit/liquid_layer.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/Misc.h>
#include <ClientFile.hpp>
#include <util/sExtendableArray.hpp>

#include <algorithm>
#include <cmath>

ChunkWater::ChunkWater(MapChunk* chunk, TileWater* water_tile, float x, float z, bool use_mclq_green_lava)
  : xbase(x)
  , zbase(z)
  , vmin(x, 0.f, z)
  , vmax(x + CHUNKSIZE, 0.f, z + CHUNKSIZE)
  , vcenter((vmin + vmax) * 0.5f)
  , _use_mclq_green_lava(use_mclq_green_lava)
  , _chunk(chunk)
  , _water_tile(water_tile)
{
}

namespace
{
  // EVERWOOD-WATER-FLOW.md reader rules: finite values, radius > 0, |dir| >= 0.01, |pos| <= 1e6.
  bool valid_flow_slot(mclq_flowvs const& f)
  {
    float const all[] = { f.pos[0], f.pos[1], f.pos[2], f.radius, f.dir[0], f.dir[1], f.dir[2]
                        , f.velocity, f.amplitude, f.frequency };
    for (float v : all)
    {
      if (!std::isfinite(v))
        return false;
    }
    if (f.radius <= 0.f)
      return false;
    if (std::sqrt(f.dir[0] * f.dir[0] + f.dir[1] * f.dir[1] + f.dir[2] * f.dir[2]) < 0.01f)
      return false;
    return std::abs(f.pos[0]) <= 1e6f && std::abs(f.pos[1]) <= 1e6f && std::abs(f.pos[2]) <= 1e6f;
  }
}

void ChunkWater::setFlow(mclq_flowvs const& slot0)
{
  _flowvs[0] = slot0;
  _flowvs[1] = mclq_flowvs{};
  _n_flowvs = 1;
}

void ChunkWater::clearFlow()
{
  _n_flowvs = 0;
  _flowvs[0] = mclq_flowvs{};
  _flowvs[1] = mclq_flowvs{};
}

void ChunkWater::from_mclq(std::vector<mclq>& layers)
{
  glm::vec3 pos(xbase, 0.0f, zbase);

  // Flow: the first layer with n_flowvs 1..2 wins; invalid slots are dropped (stock tiles have 0).
  clearFlow();
  for (mclq const& liquid : layers)
  {
    if (liquid.n_flowvs < 1 || liquid.n_flowvs > 2)
      continue;
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < liquid.n_flowvs; ++i)
    {
      if (valid_flow_slot(liquid.flowvs[i]))
        _flowvs[kept++] = liquid.flowvs[i];
    }
    if (kept)
    {
      _n_flowvs = kept;
      break;
    }
  }

  for (mclq& liquid : layers)
  {
    std::uint8_t mclq_liquid_type = 0;

    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        mclq_tile const& tile = liquid.tiles[z * 8 + x];

        misc::bit_or(attributes.fishable, x, z, tile.fishable);
        misc::bit_or(attributes.fatigue, x, z, tile.fatigue);

        if (!tile.dont_render)
        {
          mclq_liquid_type = tile.liquid_type;
        }
      }
    }

    switch (mclq_liquid_type)
    {
      case 1: // mclq ocean
        _layers.emplace_back(this, pos, liquid, LIQUID_OCEAN);
        break;
      case 3: // mclq slime
        _layers.emplace_back(this, pos, liquid, LIQUID_SLIME);
        break;
      case 4: // mclq river
        _layers.emplace_back(this, pos, liquid, LIQUID_WATER);
        break;
      case 6: // mclq magma
        _layers.emplace_back(this, pos, liquid, (_use_mclq_green_lava ? LIQUID_Green_Lava : LIQUID_MAGMA));

        break;
      default:
        // Unhandled MCLQ liquid type — overwhelmingly this is an empty/dry layer reporting type 0,
        // which is extremely common on real vanilla maps (Stormwind alone hits this thousands of
        // times). Logging per chunk floods the log and can stall/kill loading, so stay silent and
        // just don't add a water layer for it.
        break;
    }
    _water_tile->tagUpdate();
  }
  update_layers();
}

void ChunkWater::fromFile(BlizzardArchive::ClientFile &f, size_t basePos)
{
  MH2O_Header header;
  f.read(&header, sizeof(MH2O_Header));

  if (!header.nLayers)
  {
    return;
  }

  // hard sanity gate: every field below comes raw from the file and feeds seeks, reads and
  // per-layer allocations. Garbage here (truncated buffer, desynced parse, vanilla tile with a
  // junk mh2o offset) used to smash the stack via the bitmask read and corrupt the heap via
  // liquid_layer construction. A real chunk never has more than a few layers.
  if (header.nLayers > 16)
  {
    LogError << "ChunkWater: implausible MH2O layer count " << header.nLayers
             << " -- skipping water for this chunk (corrupt data)." << std::endl;
    return;
  }

  //render
  if (header.ofsAttributes)
  {
    f.seek(basePos + header.ofsAttributes);
    f.read(&attributes, sizeof(MH2O_Attributes));
  }
  else
  {
      // when chunks don't have attributes it means everything is set.
      attributes.fishable = 0xFFFFFFFFFFFFFFFF;
      attributes.fatigue = 0xFFFFFFFFFFFFFFFF;
  }

  for (std::size_t k = 0; k < header.nLayers; ++k)
  {
    MH2O_Information info;
    uint64_t infoMask = 0xFFFFFFFFFFFFFFFF; // default = all water. InfoMask + HeightMap combined

    //info
    f.seek(basePos + header.ofsInformation + sizeof(MH2O_Information)* k);
    f.read(&info, sizeof(MH2O_Information));

    // an MH2O liquid instance covers at most the 8x8 chunk grid; anything else is garbage and
    // would drive out-of-bounds math in the bitmask read and the liquid_layer constructor.
    if (info.width > 8 || info.height > 8 || info.xOffset > 8 || info.yOffset > 8)
    {
      LogError << "ChunkWater: implausible MH2O layer dims " << info.width << "x" << info.height
               << " (offset " << info.xOffset << "," << info.yOffset
               << ") -- skipping layer (corrupt data)." << std::endl;
      continue;
    }

    //mask
    if (info.ofsInfoMask > 0 && info.height > 0)
    {
      size_t bitmask_size = static_cast<size_t>(std::ceil(info.height * info.width / 8.0f));

      // infoMask is a single uint64_t: never let file-driven math write past it (stack smash)
      bitmask_size = std::min<size_t>(bitmask_size, sizeof(infoMask));

      f.seek(info.ofsInfoMask + basePos);
      // only read the relevant data
      f.read(&infoMask, bitmask_size);
    }

    glm::vec3 pos(xbase, 0.0f, zbase);
    _water_tile->tagUpdate();
    // liquid_layer(ChunkWater* chunk, BlizzardArchive::ClientFile& f, std::size_t base_pos, glm::vec3 const& base
    //              ,MH2O_Information const& info, std::uint64_t infomask);
    _layers.emplace_back(this, f, basePos, pos, info, infoMask);
  }

  update_layers();
}


void ChunkWater::save(util::sExtendableArray& adt, int base_pos, int& header_pos, int& current_pos)
{
  MH2O_Header header;

  // remove empty layers
  cleanup();
  update_attributes();

  if (hasData(0))
  {
    header.nLayers = _layer_count;;

    // fagique only for single layer ocean chunk
    bool fatigue = _layers[0].has_fatigue();
    if (!fatigue)
    {
      header.ofsAttributes = current_pos - base_pos;
      adt.Insert(current_pos, sizeof(MH2O_Attributes), reinterpret_cast<char*>(&attributes));
      current_pos += sizeof(MH2O_Attributes);
    }
    else
    {
      header.ofsAttributes = 0;
    }

    header.ofsInformation = current_pos - base_pos;
    int info_pos = current_pos;

    std::size_t info_size = sizeof(MH2O_Information) * _layer_count;
    current_pos += static_cast<std::uint32_t>(info_size);

    adt.Extend(static_cast<long>(info_size));

    for (liquid_layer& layer : _layers)
    {
      layer.save(adt, base_pos, info_pos, current_pos);
    }
  }

  memcpy(adt.GetPointer<char>(header_pos).get(), &header, sizeof(MH2O_Header));
  header_pos += sizeof(MH2O_Header);
}

void ChunkWater::prepare_mclq_save()
{
  // remove empty layers + refresh attributes BEFORE the caller sizes the MCNK -- see header.
  cleanup();
  update_attributes();
}

void ChunkWater::save_mclq(util::sExtendableArray& adt, int mcnk_pos, int& current_pos)
{
  if (hasData(0))
  {
    adt.Extend(sizeof(mclq) * _layer_count + 8);
    // size seems to be 0 in vanilla adts in the mclq chunk's header and set right in the mcnk header (layer_size * n_layer + 8)
    SetChunkHeader(adt, current_pos, 'MCLQ', 0);

    current_pos += 8;

    // it's possible to merge layers when they don't overlap (liquids using the same vertice, but at different height)
    // layer ordering seems to matter, having a lava layer then a river layer causes the lava layer to not render ingame
    // sorting order seems to be dependant on the flag ordering in the mcnk's header
    std::vector<std::pair<mclq, int>> mclq_layers;

    for (liquid_layer const& layer : _layers)
    {
      switch (layer.mclq_liquid_type())
      {
      case 6: // lava
        adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_magma = 1;
        break;
      case 3: // slime
        adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_slime = 1;
        break;
      case 1: // ocean
        adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_ocean = 1;
        break;
      default: // river
        adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_river = 1;
        break;
      }

      mclq out = layer.to_mclq(attributes);
      // every MCLQ layer of the chunk carries the chunk's flow; unused slots stay zero
      out.n_flowvs = _n_flowvs;
      for (int i = 0; i < 2; ++i)
        out.flowvs[i] = static_cast<std::uint32_t>(i) < _n_flowvs ? _flowvs[i] : mclq_flowvs{};
      mclq_layers.push_back({ out, layer.mclq_flag_ordering() });
    }

    auto cmp = [](std::pair<mclq, int> const& a, std::pair<mclq, int> const& b)
      {
        return a.second < b.second;
      };

    // sort the layers by flag order
    std::sort(mclq_layers.begin(), mclq_layers.end(), cmp);

    for (auto const& mclq_layer : mclq_layers)
    {
      std::memcpy(adt.GetPointer<char>(current_pos).get(), &mclq_layer.first, sizeof(mclq));
      current_pos += sizeof(mclq);
    }
  }
}


void ChunkWater::autoGen(MapChunk *chunk, float factor)
{
  for (liquid_layer& layer : _layers)
  {
    layer.update_opacity(chunk, factor);
  }
  update_layers();
}

void ChunkWater::update_underground_vertices_depth(MapChunk* chunk)
{
  for (liquid_layer& layer : _layers)
  {
    layer.update_underground_vertices_depth(chunk);
  }
}


void ChunkWater::CropWater(MapChunk* chunkTerrain)
{
  for (liquid_layer& layer : _layers)
  {
    layer.crop(chunkTerrain);
  }
  update_layers();
}

int ChunkWater::getType(size_t layer) const
{
  return hasData(layer) ? _layers[layer].liquidID() : 0;
}

void ChunkWater::setType(int type, size_t layer)
{
  if(hasData(layer))
  {
    _layers[layer].changeLiquidID(type);
  }
  _water_tile->tagUpdate();
}

bool ChunkWater::is_visible ( const float& cull_distance
                            , const math::frustum& frustum
                            , const glm::vec3& camera
                            , display_mode display
                            ) const
{
  static const float chunk_radius = std::sqrt (CHUNKSIZE * CHUNKSIZE / 2.0f);

  float dist = display == display_mode::in_3D
             ? (camera - vcenter).length() - chunk_radius
             : std::abs(camera.y - vmax.y);

  return frustum.intersects(vmax, vmin) && dist < cull_distance;
}

void ChunkWater::update_layers()
{
  std::size_t count = 0;

  auto& extents = _water_tile->getExtents();

  vmin.y = std::numeric_limits<float>::max();
  vmax.y = std::numeric_limits<float>::lowest();

  std::uint64_t debug_fishable_old = attributes.fishable;
  std::uint64_t debug_fatigue_old = attributes.fatigue;

  if (_auto_update_attributes)
  {
    // reset attributes
    attributes.fishable = 0;
    attributes.fatigue = 0;
  }

  for (liquid_layer& layer : _layers)
  {
    vmin.y = std::min (vmin.y, layer.min());
    vmax.y = std::max (vmax.y, layer.max());

    extents[0].y = std::min(extents[0].y, vmin.y);
    extents[1].y = std::max(extents[1].y, vmax.y);

    _water_tile->tagUpdate();

    if (_auto_update_attributes)
      layer.update_attributes(attributes);

    count++;
  }

  // some tests to compare with blizzard
  const bool run_tests = false;
  if (run_tests)
  {
    if (attributes.fishable != debug_fishable_old && _layers.size())
    {
      uint64_t x = attributes.fishable ^ debug_fishable_old; 
      int difference_count = 0;

      
      while (x > 0) {
          difference_count += x & 1;
          x >>= 1;
      }

      auto type = _layers.front().liquidID();
      std::vector<float> depths;

      int zero_depth_num = 0;
      int below_20_num = 0; // below 0.2
      for (auto& vert : _layers.front().getVertices())
      {
          depths.push_back(vert.depth);
          if (vert.depth == 0.0f)
              zero_depth_num++;
          if (vert.depth < 0.2f && vert.depth != 0.0f)
              below_20_num++;
      }
    }
  }


  _water_tile->tagExtents(true);

  vcenter = (vmin + vmax) * 0.5f;

  _layer_count = _layers.size();
}

float ChunkWater::getMinHeight() const
{
  return vmin.y;
}

float ChunkWater::getMaxHeight() const
{
  return vmax.y;
}

bool ChunkWater::hasData(size_t layer) const
{
  return _layer_count > layer;
}


void ChunkWater::paintLiquid( glm::vec3 const& pos
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
                            , int target_layer
                            , std::uint64_t surface_token
                            )
{
  // Explicit surface editing (Azure port). A selected MH2O layer is independent of
  // its liquid ID and height, which allows stacked or millimetres-apart surfaces.
  // The legacy liquid-ID brush below is reached only when target_layer < 0, so all
  // existing callers (default target_layer = -1) keep their exact behaviour.
  if (target_layer >= 0)
  {
    int effective_layer = resolve_layer(target_layer, surface_token);

    if (!add)
    {
      if (effective_layer >= 0 && effective_layer < static_cast<int>(_layers.size()))
        _layers[effective_layer].paintLiquid(pos, radius, false, angle, orientation, lock, origin,
                                             false, chunk, opacity_factor);

      cleanup();
      update_layers();
      return;
    }

    if (effective_layer < 0 || effective_layer >= static_cast<int>(_layers.size()))
    {
      liquid_layer layer(this, glm::vec3(xbase, 0.0f, zbase), lock ? origin.y : pos.y + 1.0f,
                         liquid_id);
      layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, true, chunk,
                        opacity_factor);
      layer.setSurfaceToken(surface_token);
      _water_tile->tagUpdate();
      _layers.push_back(std::move(layer));
    }
    else
    {
      liquid_layer& layer = _layers[effective_layer];
      if (override_liquid_id && layer.liquidID() != liquid_id)
        layer.changeLiquidID(liquid_id);
      layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, override_height,
                        chunk, opacity_factor);
    }

    update_layers();
    return;
  }

  if (override_liquid_id && !override_height)
  {
    bool layer_found = false;
    for (liquid_layer& layer : _layers)
    {
      if (layer.liquidID() == liquid_id)
      {
        copy_height_to_layer(layer, pos, radius);
        layer_found = true;
        break;
      }
    }

    if (!layer_found)
    {
      liquid_layer layer(this, glm::vec3(xbase, 0.0f, zbase), pos.y, liquid_id);
      copy_height_to_layer(layer, pos, radius);
      _water_tile->tagUpdate();
      _layers.push_back(layer);
    }
  }

  bool painted = false;
  for (liquid_layer& layer : _layers)
  {
    // remove the water on all layers or paint the layer with selected id
    if (!add || layer.liquidID() == liquid_id || !override_liquid_id)
    {
      layer.paintLiquid(pos, radius, add, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
      painted = true;
    }
    else
    {
      layer.paintLiquid(pos, radius, false, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    }
  }

  cleanup();

  if (!add || painted)
  {
    update_layers();
    return;
  }

  if (hasData(0))
  {
    liquid_layer layer(_layers[0]);
    layer.clear(); // remove the liquid to not override the other layer
    layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    layer.changeLiquidID(liquid_id);
    _water_tile->tagUpdate();
    _layers.push_back(layer);
  }
  else
  {
    liquid_layer layer(this, glm::vec3(xbase, 0.0f, zbase), pos.y, liquid_id);
    layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    _water_tile->tagUpdate();
    _layers.push_back(layer);
  }

  update_layers();
}

void ChunkWater::paintDepth(glm::vec3 const& pos, float radius, float depth, int target_layer,
                            std::uint64_t surface_token)
{
  int const layer = resolve_layer(target_layer, surface_token);
  if (layer < 0 || layer >= static_cast<int>(_layers.size()))
    return;

  _layers[layer].paintDepth(pos, radius, depth);
  _water_tile->tagUpdate();
  update_layers();
}

void ChunkWater::projectUV(glm::vec3 const& pos, float radius, float scale, math::radians rotation,
                           int target_layer, std::uint64_t surface_token)
{
  int const layer = resolve_layer(target_layer, surface_token);
  if (layer < 0 || layer >= static_cast<int>(_layers.size()))
    return;

  _layers[layer].projectUV(pos, radius, scale, rotation);
  _water_tile->tagUpdate();
  update_layers();
}

int ChunkWater::resolve_layer(int target_layer, std::uint64_t surface_token) const
{
  if (!surface_token)
    return target_layer;

  for (int i = 0; i < static_cast<int>(_layers.size()); ++i)
    if (_layers[i].surfaceToken() == surface_token)
      return i;

  return -1;
}

MapChunk* ChunkWater::getChunk()
{
  return _chunk;
}

TileWater* ChunkWater::getWaterTile()
{
  return _water_tile;
}

MH2O_Attributes const&  ChunkWater::getAttributes() const
{
  return attributes;
}

MH2O_Attributes& ChunkWater::getAttributes()
{
  return attributes;
}

ChunkWaterState ChunkWater::getState() const
{
  ChunkWaterState state{_layers, attributes, _auto_update_attributes};
  state.n_flowvs = _n_flowvs;
  state.flowvs[0] = _flowvs[0];
  state.flowvs[1] = _flowvs[1];
  return state;
}

void ChunkWater::restoreState(ChunkWaterState const& state)
{
  _layers = state.layers;
  // The copied layers carry the ChunkWater back-pointer from when the state was
  // captured, which may be a now-freed ChunkWater if the tile was unloaded and
  // reloaded in between. Re-point every layer at this chunk before anything
  // dereferences it (use-after-free -> poisoned Qt object otherwise).
  for (auto& layer : _layers)
    layer.setChunk(this);
  attributes = state.attributes;
  _n_flowvs = state.n_flowvs;
  _flowvs[0] = state.flowvs[0];
  _flowvs[1] = state.flowvs[1];

  // Rebuild render/extents data without replacing the restored masks.
  _auto_update_attributes = false;
  update_layers();
  _auto_update_attributes = state.auto_update_attributes;
  tagUpdate();
}

bool ChunkWater::paintAttribute(glm::vec3 const& pos, float radius, LiquidAttribute attribute,
                                bool value, int target_layer,
                                std::uint64_t surface_token)
{
  int resolved_layer = -1;
  if (target_layer >= 0)
  {
    resolved_layer = resolve_layer(target_layer, surface_token);
    if (resolved_layer < 0 || resolved_layer >= static_cast<int>(_layers.size()))
      return false;
  }

  std::uint64_t& mask = attribute == LiquidAttribute::Fishable
                      ? attributes.fishable : attributes.fatigue;
  bool changed = false;

  for (int z = 0; z < 8; ++z)
    for (int x = 0; x < 8; ++x)
    {
      if (misc::getShortestDist(pos.x, pos.z, xbase + x * UNITSIZE,
                                zbase + z * UNITSIZE, UNITSIZE) > radius)
        continue;

      bool has_liquid = false;
      if (resolved_layer >= 0)
        has_liquid = _layers[resolved_layer].hasSubchunk(x, z);
      else
        for (liquid_layer const& layer : _layers)
          if (layer.hasSubchunk(x, z))
          {
            has_liquid = true;
            break;
          }

      if (!has_liquid)
        continue;

      std::uint64_t const bit = std::uint64_t{1} << (z * 8 + x);
      bool const old_value = (mask & bit) != 0;
      if (old_value != value)
      {
        misc::set_bit(mask, x, z, value);
        changed = true;
      }
    }

  if (changed)
  {
    // Authored masks: stop update_layers() re-deriving them and drop the ocean
    // fatigue optimisation so partial fatigue edits survive in memory.
    // NOTE(azure-port): the frozen MCLQ/MH2O save paths still call
    // update_attributes() unconditionally, so these masks do NOT persist to disk
    // until save gates that call on _auto_update_attributes -- see report.
    _auto_update_attributes = false;
    for (liquid_layer& layer : _layers)
      layer.disableFatigueOptimization();
    tagUpdate();
  }
  return changed;
}

bool ChunkWater::clearAttribute(LiquidAttribute attribute)
{
  std::uint64_t& mask = attribute == LiquidAttribute::Fishable
                      ? attributes.fishable : attributes.fatigue;
  if (!mask)
    return false;

  mask = 0;
  _auto_update_attributes = false;
  for (liquid_layer& layer : _layers)
    layer.disableFatigueOptimization();
  tagUpdate();
  return true;
}

bool ChunkWater::clearAttributes()
{
  if (!attributes.fishable && !attributes.fatigue)
    return false;

  attributes.fishable = 0;
  attributes.fatigue = 0;
  _auto_update_attributes = false;
  for (liquid_layer& layer : _layers)
    layer.disableFatigueOptimization();
  tagUpdate();
  return true;
}

bool ChunkWater::clearFishableAttributesOutsideLiquid()
{
  std::uint64_t const repaired_mask = attributes.fishable & liquidCellMask();
  if (repaired_mask == attributes.fishable)
    return false;

  attributes.fishable = repaired_mask;
  _auto_update_attributes = false;
  for (liquid_layer& layer : _layers)
    layer.disableFatigueOptimization();
  tagUpdate();
  return true;
}

std::uint64_t ChunkWater::liquidCellMask() const
{
  std::uint64_t liquid_cells = 0;
  for (liquid_layer const& layer : _layers)
    liquid_cells |= layer.getSubchunks();
  return liquid_cells;
}

bool ChunkWater::regenerateAttributes()
{
  MH2O_Attributes const old_attributes = attributes;
  // Force a rebuild from liquid data even if a prior attribute edit disabled auto-update
  // (update_attributes() is now gated on _auto_update_attributes).
  _auto_update_attributes = true;
  update_attributes();
  _auto_update_attributes = false;
  bool const changed = old_attributes.fishable != attributes.fishable
                    || old_attributes.fatigue != attributes.fatigue;
  if (changed)
    tagUpdate();
  return changed;
}

int ChunkWater::layer_count() const
{
  return _layers.size();
}

void ChunkWater::cleanup()
{
  for (int i = static_cast<int>(_layer_count - 1); i >= 0; --i)
  {
    if (_layers[i].empty())
    {
      _layers.erase(_layers.begin() + i);
      _water_tile->tagUpdate();
    }
  }
  update_layers();
}

void ChunkWater::copy_height_to_layer(liquid_layer& target, glm::vec3 const& pos, float radius)
{
  for (liquid_layer& layer : _layers)
  {
    if (layer.liquidID() == target.liquidID())
    {
      continue;
    }

    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (misc::getShortestDist(pos.x, pos.z, xbase + x*UNITSIZE, zbase + z*UNITSIZE, UNITSIZE) <= radius)
        {
          if (layer.hasSubchunk(x, z))
          {
            target.copy_subchunk_height(x, z, layer);
          }
        }
      }
    }
  }
}

void ChunkWater::update_attributes()
{
  // Authored fishable/fatigue masks (paintAttribute/clear*) set _auto_update_attributes to
  // false so this re-derivation stops clobbering them. The frozen MCLQ/MH2O save paths call
  // this unconditionally; gating here keeps an ordinary save byte-identical (the flag stays
  // true for normal painting) while letting hand-authored masks persist to disk. The only
  // forced run is regenerateAttributes(), which sets the flag true around its call. (azure-port)
  if (!_auto_update_attributes)
    return;

  attributes.fishable = 0;
  attributes.fatigue = 0;

  for (liquid_layer& layer : _layers)
  {
      layer.update_attributes(attributes);
  }
}

void ChunkWater::tagUpdate()
{
  _water_tile->tagUpdate();
}

std::vector<liquid_layer>* ChunkWater::getLayers()
{
  return &_layers;
}

std::vector<liquid_layer>* ChunkWater::getRenderLayers()
{
  return _chunk_mover_preview_layers ? &*_chunk_mover_preview_layers : &_layers;
}

void ChunkWater::setChunkMoverLiquidPreview(std::optional<std::vector<liquid_layer>> layers)
{
  _chunk_mover_preview_layers = std::move(layers);
  // Preview layers are built from the SOURCE chunks, so their back-pointer points
  // elsewhere; re-point at this chunk (the one rendering the ghost) so nothing
  // dereferences a source chunk that may since have unloaded.
  if (_chunk_mover_preview_layers)
    for (auto& layer : *_chunk_mover_preview_layers)
      layer.setChunk(this);
  tagUpdate();
}

