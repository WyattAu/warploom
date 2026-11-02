#version 450

// bloom_downsample_pt.frag — 13-tap Karis-average downsample for bloom over a
// PATH-TRACED accumulation (PT + bloom layering).
//
// Same filter and threshold as bloom_downsample.frag, but the source is the
// tracer's accumulation SUM (a GENERAL-layout storage image), not a radiance
// intermediate. The sum grows with the sample count, so the threshold would
// be meaningless without the divide: this shader multiplies by
// inverse_samples (1/n, the same push the tonemap uses) BEFORE the Karis
// average, so the threshold sees the same mean radiance the raster bloom
// thresholds. Exposure stays in the tonemap only, exactly as on the raster
// path — the bloom signal is pre-exposure radiance in both.
//
// A separate file (not a branch in bloom_downsample.frag) keeps the raster
// shader — and every hash pinned on it — bit-identical.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_image;

// Same 16-byte Compose block as the tonemap (exposure unused here, kept so
// both stages share one push range shape and the caller pushes one struct).
layout(push_constant) uniform Compose {
  float exposure;
  float inverse_samples;
  float pad[2];
} pc;

const float kThreshold = 0.8;  // brightness threshold for bloom extraction

void main() {
  vec2 texel = 1.0 / vec2(textureSize(src_image, 0));
  vec3 color = vec3(0.0);

  // 13-tap Karis filter over the accumulation sums, divided to means first.
  // (Epic Games / Call of Duty approach, same taps as bloom_downsample.)
  vec3 center = texture(src_image, v_uv).rgb * pc.inverse_samples;
  color += center * 4.0;

  color += texture(src_image, v_uv + vec2( 0.0, -texel.y) * 2.0).rgb * pc.inverse_samples * 2.0;
  color += texture(src_image, v_uv + vec2( 0.0,  texel.y) * 2.0).rgb * pc.inverse_samples * 2.0;
  color += texture(src_image, v_uv + vec2(-texel.x,  0.0) * 2.0).rgb * pc.inverse_samples * 2.0;
  color += texture(src_image, v_uv + vec2( texel.x,  0.0) * 2.0).rgb * pc.inverse_samples * 2.0;

  color += texture(src_image, v_uv + vec2(-texel.x, -texel.y) * 2.0).rgb * pc.inverse_samples;
  color += texture(src_image, v_uv + vec2( texel.x, -texel.y) * 2.0).rgb * pc.inverse_samples;
  color += texture(src_image, v_uv + vec2(-texel.x,  texel.y) * 2.0).rgb * pc.inverse_samples;
  color += texture(src_image, v_uv + vec2( texel.x,  texel.y) * 2.0).rgb * pc.inverse_samples;

  color += texture(src_image, v_uv + vec2(-texel.x * 2.0, 0.0)).rgb * pc.inverse_samples;
  color += texture(src_image, v_uv + vec2( texel.x * 2.0, 0.0)).rgb * pc.inverse_samples;
  color += texture(src_image, v_uv + vec2(0.0, -texel.y * 2.0)).rgb * pc.inverse_samples;
  color += texture(src_image, v_uv + vec2(0.0,  texel.y * 2.0)).rgb * pc.inverse_samples;

  color /= 16.0;

  // Extract bright pixels.
  float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
  color *= smoothstep(kThreshold, kThreshold + 0.2, brightness);

  out_color = vec4(color, 1.0);
}
