#version 450

// Samples one combined image sampler at the interpolated UV and writes the
// texel color straight through (nearest filtering in the test harness keeps
// each output texel equal to its source texel).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D albedo;

void main() {
  out_color = texture(albedo, v_uv);
}
