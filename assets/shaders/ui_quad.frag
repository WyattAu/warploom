#version 450

// ui_quad.frag — sample the glyph atlas: alpha = sampled alpha (the solid
// cell is 1.0 everywhere, glyphs are antialiasing-free bit patterns), and
// multiply the vertex color through. Alpha blending is a follow-up; the
// UI paints opaque quads (alpha 255) so out.a = 1 keeps the render pass
// correct with the engine's no-blend pipeline helper.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D atlas;

void main() {
  float a = texture(atlas, v_uv).a;
  out_color = vec4(v_color.rgb, 1.0) * a;
}
