#version 450

// shadow.frag — empty fragment stage for the depth-only shadow pass.
// The depth value is written by the fixed-function depth test; this shader
// exists only so Vulkan has a fragment stage for the graphics pipeline.

layout(location = 0) out vec4 out_color;

void main() {
  // Depth is written automatically. Nothing to output.
  out_color = vec4(0.0);
}
