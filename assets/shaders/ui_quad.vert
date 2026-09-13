#version 450

// ui_quad.vert — M4 UI paint-list rendering. One vertex = one corner of a
// UI quad, all data pulled from the vertex buffer (no gl_VertexIndex math):
//   pos    pixel coordinates, y-down (paint-list frame: row 0 = top)
//   uv     glyph-atlas texel coordinates (rects use the solid-white cell)
//   color  quad color (0xAARRGGBB packed by the CPU, expanded here)
// Clip conversion: x: 2*x/w - 1; y: 1 - 2*y/h (keeps row 0 at the top of the
// framebuffer so readback row order matches the paint list exactly).

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_color;

layout(push_constant) uniform Pc {
  vec2 resolution;  // viewport in pixels
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
  // Vulkan NDC is y-down (framebuffer origin top-left): pixel row 0 maps to
  // NDC y = -1, so the paint-list frame maps 1:1 onto the framebuffer.
  vec2 ndc = vec2(2.0 * in_pos.x / pc.resolution.x - 1.0,
                  2.0 * in_pos.y / pc.resolution.y - 1.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  v_uv = in_uv;
  v_color = vec4(float(in_color & 0xFFu), float((in_color >> 8) & 0xFFu),
                 float((in_color >> 16) & 0xFFu),
                 float((in_color >> 24) & 0xFFu)) / 255.0;
}
