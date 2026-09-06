#version 450

// Fullscreen textured quad: a single triangle in clip space that covers the
// viewport. UVs are chosen so v_uv == (0,0) at the top-left framebuffer
// pixel and (1,1) at the bottom-right, matching PNG row order (row 0 = top)
// after the y-flip of the clip->framebuffer transform.

layout(location = 0) out vec2 v_uv;

void main() {
  // Clip-space fullscreen triangle (no vertex buffer).
  const vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  // Pixel (u,v): u = (clip.x + 1)/2, v = (clip.y + 1)/2 (row 0 of the image
  // appears at the bottom of the clip-space mapping; the harness verifies
  // orientation empirically against PNG row order).
  const vec2 uvs[3] = vec2[](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));
  gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
  v_uv = uvs[gl_VertexIndex];
}
