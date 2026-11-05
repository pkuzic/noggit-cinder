// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_WORLDRENDER_HPP
#define NOGGIT_WORLDRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>
#include <noggit/TileIndex.hpp>

#include <external/glm/glm.hpp>

#include <noggit/tool_enums.hpp>
#include <noggit/rendering/CursorRender.hpp>
#include <noggit/rendering/LiquidTextureManager.hpp>
#include <noggit/rendering/DetailDoodadRender.hpp>
#include <noggit/DetailDoodads.hpp>
#include <noggit/map_horizon.h>
#include <noggit/Sky.h>

#include <noggit/rendering/Primitives.hpp>
#include <noggit/ModelManager.h> // scoped_model_reference

#include <memory>
#include <optional>
#include <vector>
#include <map>
#include <array>
#include <algorithm>
#include <cstdint>

namespace OpenGL
{
  struct program;
}

struct TileIndex;
class World;
struct MinimapRenderSettings;


struct WorldRenderParams 
{
  float cursorRotation;
  CursorType cursor_type;
  // Water editor overlay/brush state fed from the active tool's draw parameters (Azure port).
  bool project_cursor_on_water = false;
  bool show_liquid_vertices = false;
  int liquid_attribute_overlay = 0;
  int liquid_edit_layer = -1;
  std::uint64_t liquid_surface_token = 0;
  int liquid_brush_falloff = 1;
  float brush_radius;
  bool show_unpaintable_chunks;
  bool show_painted_stamp_selection = false;
  bool draw_only_inside_light_sphere;
  bool draw_wireframe_light_sphere;
  float alpha_light_sphere;
  float inner_radius_ratio;
  float angle;
  float orientation;
  bool use_ref_pos;
  bool angled_mode;
  bool draw_paintability_overlay;
  editing_mode editing_mode;
  bool camera_moved;
  bool draw_mfbo;
  bool draw_terrain;
  bool draw_wmo;
  bool draw_water;
  bool draw_wmo_doodads;
  bool draw_models;
  bool draw_model_animations;
  bool draw_models_with_box;
  bool draw_hidden_models;
  bool draw_sky;
  bool draw_skybox;
  bool draw_fog;
  eTerrainType ground_editing_brush;
  int water_layer;
  display_mode display_mode;
  bool draw_occlusion_boxes;
  bool minimap_render;
  bool draw_wmo_exterior;

  bool render_select_m2_aabb;
  bool render_select_m2_collission_bbox;
  bool render_select_wmo_aabb;
  bool render_select_wmo_groups_bounds;

  // Road sub-mode viewport preview (ported from Noggit Azure): the baked route and the
  // blue reference-selection overlay, drawn as translucent lines over the terrain.
  std::vector<glm::vec3> road_preview_centerline;
  std::vector<glm::vec3> road_preview_left_edge;
  std::vector<glm::vec3> road_preview_right_edge;
  bool road_preview_blocked = false;
  std::vector<glm::vec3> road_reference_centerline;
  std::vector<glm::vec3> road_reference_left_edge;
  std::vector<glm::vec3> road_reference_right_edge;
  std::vector<std::vector<glm::vec3>> road_reference_mask_lines;
};

namespace Noggit::Everwood
{
  class EverwoodLayer;
}

namespace Noggit::Rendering
{
  class WorldRender : public BaseRender
  {
  public:
    WorldRender(World* world);

    // The Everwood voxel layer of this map, null outside Everwood Mode (then nothing of it runs).
    void setEverwoodLayer(Noggit::Everwood::EverwoodLayer* layer) { _everwood_layer = layer; }
    Noggit::Everwood::EverwoodLayer* everwoodLayer() const { return _everwood_layer; }

    void upload() override;
    void unload() override;

    // Painted-stamp source-selection overlay (ported from Noggit Azure): highlights the stamp
    // SOURCE chunks in the 3D viewport. update/clear are driven by the stamp tool (BrushStack);
    // prepare uploads pending mask changes once per frame before the terrain pass.
    void updatePaintedStampSelectionOverlay(std::vector<glm::ivec2> const& cells, bool selected);
    void clearPaintedStampSelectionOverlay();
    void preparePaintedStampSelectionOverlay();

    // in-editor detail-doodad live preview (client ground-clutter preview), ported from Noggit Azure
    void setDetailDoodadPreview(Noggit::DetailDoodadPreview preview);
    void clearDetailDoodadPreview();

    void draw (glm::mat4x4 const& model_view
        , glm::mat4x4 const& projection
        , glm::vec3 const& cursor_pos
        , glm::vec4 const& cursor_color
        , glm::vec3 const& ref_pos
        , glm::vec3 const& camera_pos
        , MinimapRenderSettings* minimap_render_settings
        , WorldRenderParams const& render_settings
    );

    bool saveMinimap (TileIndex const& tile_idx
                      , MinimapRenderSettings* settings
                      , std::optional<QImage>& combined_image);

    [[nodiscard]]
    OpenGL::TerrainParamsUniformBlock* getTerrainParamsUniformBlock();;

    void updateTerrainParamsUniformBlock();
    void markTerrainParamsUniformBlockDirty();;

    [[nodiscard]] std::unique_ptr<Skies>& skies();;

    float _view_distance;
    float cullDistance() const;

    unsigned int _frame_max_chunk_updates = 256;

    bool directional_lightning;
    bool local_lightning;

  private:

    // error-cube substitutes for models/WMOs that failed to load; created lazily on first use
    std::optional<scoped_model_reference> _missing_m2_placeholder;
    std::optional<scoped_model_reference> _missing_wmo_placeholder;

    void drawMinimap ( MapTile *tile
        , glm::mat4x4 const& model_view
        , glm::mat4x4 const& projection
        , glm::vec3 const& camera_pos
        , MinimapRenderSettings* settings
    );

    void updateMVPUniformBlock(const glm::mat4x4& model_view, const glm::mat4x4& projection);
    void updateLightingUniformBlock(bool draw_fog, glm::vec3 const& camera_pos);
    void updateLightingUniformBlockMinimap(MinimapRenderSettings* settings);

    void setupChunkVAO(OpenGL::Scoped::use_program& mcnk_shader);
    void setupLiquidChunkVAO(OpenGL::Scoped::use_program& water_shader);
    void setupOccluderBuffers();
    void setupChunkBuffers();
    void setupLiquidChunkBuffers();

    World* _world;
    float _cull_distance;

    // shaders
    std::unique_ptr<OpenGL::program> _mcnk_program;;
    std::unique_ptr<OpenGL::program> _mfbo_program;
    std::unique_ptr<OpenGL::program> _m2_program;
    std::unique_ptr<OpenGL::program> _m2_instanced_program;
    std::unique_ptr<OpenGL::program> _m2_particles_program;
    std::unique_ptr<OpenGL::program> _m2_ribbons_program;
    std::unique_ptr<OpenGL::program> _m2_box_program;
    std::unique_ptr<OpenGL::program> _wmo_program;
    std::unique_ptr<OpenGL::program> _liquid_program;
    std::unique_ptr<OpenGL::program> _occluder_program;
    std::unique_ptr<OpenGL::program> _detail_doodads_program;

    // ground-effect detail doodads (client ground clutter preview), ported from Noggit Azure
    Noggit::Rendering::DetailDoodadRender _detail_doodads;
    Noggit::DetailDoodadPreview _detail_doodad_preview;
    // OFF by default: this preview generates+loads clutter models on the render thread and can
    // throw (e.g. bad_alloc under the model volume of a full Blizzard continent). Opt-in via the
    // QSettings key "draw_detail_doodads"; the render hook also self-disables on any exception so
    // it can never crash the app. Re-enable by default once verified safe on large maps.
    bool _draw_detail_doodads = false;
    int _detail_doodad_density = 16;    // client CVar groundEffectDensity, 16..256
    float _detail_doodad_distance = 140.f;

    // painted-stamp source-selection overlay (ported from Noggit Azure): a per-tile R8 mask
    // texture, double-buffered so an upload never clobbers the texture the terrain shader may
    // still be sampling, bound to texture unit 4 while drawing terrain.
    static constexpr int painted_stamp_selection_resolution = 1024;
    struct PaintedStampSelectionDirtyRegion
    {
      int min_x = painted_stamp_selection_resolution;
      int min_z = painted_stamp_selection_resolution;
      int max_x = -1;
      int max_z = -1;

      [[nodiscard]] bool pending() const
      {
        return max_x >= min_x && max_z >= min_z;
      }

      void include(int x, int z)
      {
        min_x = std::min(min_x, x);
        min_z = std::min(min_z, z);
        max_x = std::max(max_x, x);
        max_z = std::max(max_z, z);
      }

      void includeAll()
      {
        min_x = 0;
        min_z = 0;
        max_x = painted_stamp_selection_resolution - 1;
        max_z = painted_stamp_selection_resolution - 1;
      }

      void clear()
      {
        min_x = painted_stamp_selection_resolution;
        min_z = painted_stamp_selection_resolution;
        max_x = -1;
        max_z = -1;
      }
    };

    struct PaintedStampSelectionPage
    {
      std::vector<std::uint8_t> pixels;
      std::size_t selected_count = 0;
      std::array<GLuint, 2> textures{};
      std::array<bool, 2> uploaded{};
      std::array<PaintedStampSelectionDirtyRegion, 2> dirty_regions{};
      int displayed_texture = -1;
      bool needs_upload = false;
    };

    bool bindPaintedStampSelectionOverlay(TileIndex const& tile_index);
    std::map<TileIndex, PaintedStampSelectionPage> _painted_stamp_selection_pages;

    // horizon && skies && lighting
    std::unique_ptr<Noggit::map_horizon::render> _horizon_render;
    std::unique_ptr<OutdoorLighting> _outdoor_lighting;
    OutdoorLightStats _outdoor_light_stats;
    std::unique_ptr<Skies> _skies;

    // cursor
    Noggit::CursorRender _cursor_render;
    Noggit::Rendering::Primitives::Sphere _sphere_render;
    Noggit::Rendering::Primitives::Square _square_render;
    Noggit::Rendering::Primitives::Line _line_render;
    Noggit::Rendering::Primitives::WireBox _wirebox_render;

    // buffers
    OpenGL::Scoped::deferred_upload_buffers<8> _buffers;
    GLuint const& _mvp_ubo = _buffers[0];
    GLuint const& _lighting_ubo = _buffers[1];
    Noggit::Everwood::EverwoodLayer* _everwood_layer = nullptr;

    GLuint const& _terrain_params_ubo = _buffers[2];
    GLuint const& _mapchunk_vertex = _buffers[3];
    GLuint const& _mapchunk_index = _buffers[4];
    GLuint const& _mapchunk_texcoord = _buffers[5];
    GLuint const& _liquid_chunk_vertex = _buffers[6];
    GLuint const& _occluder_index = _buffers[7];

    // uniform blocks
    OpenGL::MVPUniformBlock _mvp_ubo_data;
    OpenGL::LightingUniformBlock _lighting_ubo_data;
    OpenGL::TerrainParamsUniformBlock _terrain_params_ubo_data;

    // VAOs
    OpenGL::Scoped::deferred_upload_vertex_arrays<3> _vertex_arrays;
    GLuint const& _mapchunk_vao = _vertex_arrays[0];
    GLuint const& _liquid_chunk_vao = _vertex_arrays[1];
    GLuint const& _occluder_vao = _vertex_arrays[2];

    LiquidTextureManager _liquid_texture_manager;

    bool _need_terrain_params_ubo_update = false;
  };
}

#endif //NOGGIT_WORLDRENDER_HPP
