#version 450

// fullscreen.vert — generates a screen-filling triangle from gl_VertexIndex.
// No vertex buffer needed. Outputs UV for fragment sampling.

layout(location = 0) out vec2 v_uv;

void main() {
  // Generate a single triangle that covers the entire screen.
  // gl_VertexIndex: 0 = (-1,-1), 1 = (3,-1), 2 = (-1,3)
  // This produces a triangle that covers [-1,1] x [-1,1] with UV [0,1].
  float x = float((gl_VertexIndex & 1) << 2) - 1.0;
  float y = float((gl_VertexIndex & 2)) - 1.0;
  v_uv = vec2(x, y) * 0.5 + 0.5;
  gl_Position = vec4(x, y, 0.0, 1.0);
}
