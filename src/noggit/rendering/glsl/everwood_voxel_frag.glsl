// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// The Everwood voxel mesh: the terrain shader's layer blend, vertex colour, lighting, baked shadow, grid lines,
// fog and brush cursor, plus a wall look for edited faces that turned away from the ground (pit walls,
// undersides), CONTRACT v3.3.6. vary_normal is in the terrain's convention: WoW x, up, WoW y.
#version 410 core

layout (std140) uniform lighting
{
  vec4 DiffuseColor_FogStart;
  vec4 AmbientColor_FogEnd;
  vec4 FogColor_FogOn;
  vec4 LightDir_FogRate;
  vec4 OceanColorLight;
  vec4 OceanColorDark;
  vec4 RiverColorLight;
  vec4 RiverColorDark;
};

layout (std140) uniform overlay_params
{
  int draw_shadows;
  int draw_lines;
  int draw_hole_lines;
  int draw_areaid_overlay;
  int draw_terrain_height_contour;
  int draw_wireframe;
  int wireframe_type;
  float wireframe_radius;
  float wireframe_width;
  int draw_impass_overlay;
  int draw_paintability_overlay;
  int draw_selection_overlay;
  vec4 wireframe_color;
  int draw_impassible_climb;
  int climb_use_output_angle;
  int climb_use_smooth_interpolation;
  float climb_value;
  int draw_vertex_color;
  int draw_groundeffectid_overlay;
  int draw_groundeffect_layerid_overlay;
  int draw_noeffectdoodad_overlay;
  int draw_only_normals;
  int point_normals_up;
};

uniform sampler2DArray textures[11];
uniform sampler2D stamp_brush;
uniform vec3 camera;

uniform int draw_cursor_circle;
uniform vec3 cursor_position;
uniform float cursorRotation;
uniform float outer_cursor_radius;
uniform float inner_cursor_ratio;
uniform vec4 cursor_color;

in vec3 vary_position;
in vec3 vary_normal;
in vec2 vary_uv;
in vec3 vary_alpha;
in float vary_wall;
flat in vec4 vary_layers;
in vec3 vary_mccv;
in float vary_shadow;

out vec4 out_color;

const float TILESIZE = 533.33333;
const float CHUNKSIZE = TILESIZE / 16.0;
const float HOLESIZE = CHUNKSIZE * 0.25;
const float UNITSIZE = HOLESIZE * 0.5;
const float PI = 3.14159265358979323846;
const int LAYER_SPECULAR = 1 << 20;

vec4 sample_layer(int unit, vec3 c)
{
  if (unit == 0) return texture(textures[0], c);
  if (unit == 1) return texture(textures[1], c);
  if (unit == 2) return texture(textures[2], c);
  if (unit == 3) return texture(textures[3], c);
  if (unit == 4) return texture(textures[4], c);
  if (unit == 5) return texture(textures[5], c);
  if (unit == 6) return texture(textures[6], c);
  if (unit == 7) return texture(textures[7], c);
  if (unit == 8) return texture(textures[8], c);
  if (unit == 9) return texture(textures[9], c);
  return texture(textures[10], c);
}

vec4 layer_color(vec2 uv, float encoded)
{
  int id = int(encoded);
  if (id < 0)
  {
    return vec4(0.0);
  }

  bool specular = (id & LAYER_SPECULAR) != 0;
  id &= LAYER_SPECULAR - 1;
  vec4 color = sample_layer(id / 4096, vec3(uv, float(id % 4096)));
  // As the terrain: only a specular tileset keeps its alpha as a specular mask.
  color.a = specular ? color.a : 0.0;
  return color;
}

float contour_alpha(float unit_size, float pos, float line_width)
{
  float f = abs(fract((pos + unit_size * 0.5) / unit_size) - 0.5);
  float df = abs(line_width / unit_size);
  return smoothstep(0.0, df, f);
}

float contour_alpha(float unit_size, vec2 pos, vec2 line_width)
{
  return 1.0 - min(contour_alpha(unit_size, pos.x, line_width.x), contour_alpha(unit_size, pos.y, line_width.y));
}

vec4 blend(vec2 uv)
{
  if (vary_layers.x < 0.0)
  {
    return vec4(1.0, 1.0, 1.0, 0.0);   // a chunk without layers, as the terrain draws it
  }

  float a0 = vary_alpha.x;
  float a1 = vary_alpha.y;
  float a2 = vary_alpha.z;
  return layer_color(uv, vary_layers.x) * (1.0 - (a0 + a1 + a2))
       + layer_color(uv, vary_layers.y) * a0
       + layer_color(uv, vary_layers.z) * a1
       + layer_color(uv, vary_layers.w) * a2;
}

void main()
{
  float dist_from_camera = distance(camera, vary_position);

  vec3 fw = fwidth(vary_position);

  vec3 normalized_normal = normalize(vary_normal);
  if (point_normals_up == 1)
  {
    normalized_normal = vec3(0.0, 1.0, 0.0);
  }
  float nDotL = clamp(dot(normalized_normal, -normalize(LightDir_FogRate.xyz)), 0.0, 1.0);

  vec3 skyColor = (AmbientColor_FogEnd.xyz * 1.10000002);
  vec3 groundColor = (AmbientColor_FogEnd.xyz * 0.699999988);

  vec3 currColor = mix(groundColor, skyColor, 0.5 + (0.5 * nDotL));
  vec3 lDiffuse = DiffuseColor_FogStart.xyz * nDotL;

  vec3 reflection = normalize(normalized_normal - (-LightDir_FogRate.xyz));
  float specularFactor = max(dot(reflection, normalize(camera - vary_position)), 0.0);

  vec4 ground = blend(vary_uv);
  float wall = clamp(vary_wall, 0.0, 1.0);
  if (wall > 0.0)
  {
    // Planar on the side that faces the wall: the ground's mapping would stretch to streaks here, and the
    // ground's own colours read as earth once they are darkened and warmed.
    vec3 n = abs(normalized_normal);
    // n.x is along WoW x, which is Noggit's z.
    vec2 side = (n.z > n.x ? vary_position.zy : vary_position.xy) / UNITSIZE;
    vec4 rock = blend(side);
    float luminance = dot(rock.rgb, vec3(0.299, 0.587, 0.114));
    vec3 earth = vec3(0.70, 0.56, 0.42) * (0.55 + 1.0 * luminance);
    ground = mix(ground, vec4(earth, 0.0), wall);
    // A wall rarely faces the sun: without this every pit reads as a black hole.
    currColor += AmbientColor_FogEnd.xyz * 0.45 * wall;
  }

  out_color = ground;
  vec3 spc = out_color.a * out_color.rgb * pow(specularFactor, 8);
  out_color.a = 1.0;

  if (draw_vertex_color != 0)
  {
    out_color.rgb *= vary_mccv;
  }

  out_color.rgb = clamp(out_color.rgb * (currColor + lDiffuse + spc), 0.0, 1.0);

  if (draw_shadows != 0)
  {
    out_color.rgb *= 1.0 - vary_shadow;
  }

  if (draw_terrain_height_contour != 0)
  {
    out_color.rgb *= contour_alpha(4.0, vary_position.y + 0.1, fw.y);
  }

  if (draw_lines != 0)
  {
    vec4 color = vec4(0.0);
    color.a = contour_alpha(TILESIZE, vary_position.xz, fw.xz * 1.5);
    color.g = color.a > 0.0 ? 0.8 : 0.0;
    if (color.a == 0.0)
    {
      color.a = contour_alpha(CHUNKSIZE, vary_position.xz, fw.xz);
      color.r = color.a > 0.0 ? 0.8 : 0.0;
    }
    if (draw_hole_lines != 0 && color.a == 0.0)
    {
      color.a = contour_alpha(HOLESIZE, vary_position.xz, fw.xz * 0.75);
      color.b = 0.8;
    }
    out_color.rgb = mix(out_color.rgb, color.rgb, color.a);
  }

  if (FogColor_FogOn.w != 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;
    float rate = -(1.0 / (AmbientColor_FogEnd.w - start));
    float offset = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    float f = min(pow(max((dist_from_camera * rate) + offset, 0.0), LightDir_FogRate.w), 1.0);
    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, 1.0 - f);
  }

  if (draw_cursor_circle == 1)
  {
    float diff = length(vary_position.xz - cursor_position.xz);
    diff = min(abs(diff - outer_cursor_radius), abs(diff - outer_cursor_radius * inner_cursor_ratio));
    out_color.rgb = mix(cursor_color.rgb, out_color.rgb, smoothstep(0.0, length(fw.xz), diff));
  }
  else if (draw_cursor_circle == 2)
  {
    float angle = cursorRotation * 2.0 * PI;
    vec2 topleft = cursor_position.xz - vec2(outer_cursor_radius);
    vec2 texcoord = (vary_position.xz - topleft) / outer_cursor_radius * 0.5 - 0.5;
    vec2 rotated = vec2(texcoord.x * cos(angle) + texcoord.y * sin(angle) + 0.5,
                        texcoord.y * cos(angle) - texcoord.x * sin(angle) + 0.5);
    out_color.rgb = mix(out_color.rgb, cursor_color.rgb, texture(stamp_brush, rotated).r
      * int(abs(vary_position.x - cursor_position.x) <= outer_cursor_radius
         && abs(vary_position.z - cursor_position.z) <= outer_cursor_radius));
  }

  if (draw_only_normals != 0)
  {
    out_color.rgb = vec3(vary_normal.z * -1.0, vary_normal.y, vary_normal.x * -1.0) * 0.5 + 0.5;
  }
}
