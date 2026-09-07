#version 450

// bloom_upsample.frag — tent filter upsample for bloom.
// Reads a smaller-resolution bloom texture and bilinearly upsamples,
// blending additively with the target.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D bloom_tex;

void main() {
  // Simple bilinear upsample — the tent filter is implicit in the sampler.
  vec3 bloom = texture(bloom_tex, v_uv).rgb;
  out_color = vec4(bloom, 0.0);
}
