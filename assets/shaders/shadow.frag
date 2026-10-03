#version 450

// shadow.frag — empty fragment stage for the depth-only shadow pass.
// The depth value is written by the fixed-function depth test; this shader
// exists only so Vulkan has a fragment stage for the graphics pipeline.
//
// It deliberately declares NO color outputs. The shadow render pass has no
// color attachment, and writing to a location with no matching
// pColorAttachments[0] is a validation error
// (VUID-StandaloneSpirv-Fragment-Output-06304).

void main() {
  // Nothing to shade and nothing to write.
}