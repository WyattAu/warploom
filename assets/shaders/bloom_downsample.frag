#version 450

// bloom_downsample.frag — 13-tap Karis-average downsample for bloom.
// Extracts bright pixels (luma > threshold) and downsamples by 2x.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_image;

const float kThreshold = 0.8;  // brightness threshold for bloom extraction

void main() {
  vec2 texel = 1.0 / vec2(textureSize(src_image, 0));
  vec3 color = vec3(0.0);

  // 13-tap Karis filter (Epic Games / Call of Duty approach).
  // Center
  color += texture(src_image, v_uv).rgb * 4.0;

  // 4 direct neighbors
  color += texture(src_image, v_uv + vec2( 0.0, -texel.y) * 2.0).rgb * 2.0;
  color += texture(src_image, v_uv + vec2( 0.0,  texel.y) * 2.0).rgb * 2.0;
  color += texture(src_image, v_uv + vec2(-texel.x,  0.0) * 2.0).rgb * 2.0;
  color += texture(src_image, v_uv + vec2( texel.x,  0.0) * 2.0).rgb * 2.0;

  // 4 diagonals
  color += texture(src_image, v_uv + vec2(-texel.x, -texel.y) * 2.0).rgb;
  color += texture(src_image, v_uv + vec2( texel.x, -texel.y) * 2.0).rgb;
  color += texture(src_image, v_uv + vec2(-texel.x,  texel.y) * 2.0).rgb;
  color += texture(src_image, v_uv + vec2( texel.x,  texel.y) * 2.0).rgb;

  // 4 outer samples
  color += texture(src_image, v_uv + vec2(-texel.x * 2.0, 0.0)).rgb;
  color += texture(src_image, v_uv + vec2( texel.x * 2.0, 0.0)).rgb;
  color += texture(src_image, v_uv + vec2(0.0, -texel.y * 2.0)).rgb;
  color += texture(src_image, v_uv + vec2(0.0,  texel.y * 2.0)).rgb;

  color /= 16.0;

  // Extract bright pixels.
  float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
  color *= smoothstep(kThreshold, kThreshold + 0.2, brightness);

  out_color = vec4(color, 1.0);
}
