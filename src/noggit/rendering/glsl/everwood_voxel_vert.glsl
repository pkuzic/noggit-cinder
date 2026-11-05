// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 texcoord;
layout (location = 3) in vec3 alpha;
layout (location = 4) in float wall;
layout (location = 5) in vec4 layers;

layout (std140) uniform matrices
{
  mat4 model_view;
  mat4 projection;
};

out vec3 vary_position;
out vec3 vary_normal;
out vec2 vary_uv;
out vec3 vary_alpha;
out float vary_wall;
flat out vec4 vary_layers;

void main()
{
  gl_Position = projection * model_view * vec4(position, 1.0);
  vary_position = position;
  vary_normal = normal;
  vary_uv = texcoord;
  vary_alpha = alpha;
  vary_wall = wall;
  vary_layers = layers;
}
