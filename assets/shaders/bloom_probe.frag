#version 450

// bloom_probe.frag — scene fragment stage for the runtime bloom-toggle probe
// (VulkanHardware.HeadlessComposeFrameGoldenHash).
//
// It exists because bloom is a bright-pass: bloom_downsample.frag multiplies
// by smoothstep(0.8, 1.0, luma), so a scene whose brightest pixel is below 0.8
// produces IDENTICAL output whether bloom is on or off. The probe scene (the
// test triangle) peaks at about 0.29 luma, which made "the toggle changes the
// frame" untestable — and an untestable toggle is exactly how the original
// black-frame bloom bug survived: every prior check was "does it run without
// VUIDs", never "are the pixels right".
//
// Paired with triangle.vert, whose interpolated colors are all <= 1.0. 8x puts
// the brightest pixel near 2.4 luma, comfortably over the threshold while
// staying inside what ACES maps to something visible.

layout(location = 0) in vec3 color;
layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(color * 8.0, 1.0);
}
