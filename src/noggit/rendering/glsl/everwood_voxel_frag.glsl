// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// The Everwood voxel mesh: the terrain shader's layer blend, lighting and fog, plus a wall look for edited
// faces that turned away from the ground (pit walls, undersides), CONTRACT v3.3.6.
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

uniform sampler2DArray textures[11];
uniform vec3 camera;

in vec3 vary_position;
in vec3 vary_normal;
in vec2 vary_uv;
in vec3 vary_alpha;
in float vary_wall;
flat in vec4 vary_layers;

out vec4 out_color;

const float UNITSIZE = 533.33333 / 16.0 / 8.0;

vec4 layer_color(vec2 uv, float encoded)
{
  int id = int(encoded);
  if (id < 0)
  {
    return vec4(0.0);
  }

  int unit = id / 4096;
  float index = float(id % 4096);
  vec3 c = vec3(uv, index);

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

vec4 blend(vec2 uv)
{
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

  vec3 normalized_normal = normalize(vary_normal);
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
    vec2 side = (n.x > n.z ? vary_position.zy : vary_position.xy) / UNITSIZE;
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
  out_color.rgb = clamp(out_color.rgb * (currColor + lDiffuse + spc), 0.0, 1.0);

  if (FogColor_FogOn.w != 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;
    float rate = -(1.0 / (AmbientColor_FogEnd.w - start));
    float offset = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    float f = min(pow(max((dist_from_camera * rate) + offset, 0.0), LightDir_FogRate.w), 1.0);
    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, 1.0 - f);
  }
}
