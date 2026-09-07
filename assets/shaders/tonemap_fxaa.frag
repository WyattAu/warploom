#version 450

// tonemap_fxaa.frag — ACES filmic tonemapping + FXAA anti-aliasing.
// Input: HDR linear RGB texture (set 0, binding 0).
// Output: LDR sRGB tonemapped + FXAA-smoothed color.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D hdr_image;

// ACES filmic tonemapping (Narkowicz 2015).
vec3 aces_tonemap(vec3 x) {
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// FXAA 3.11 (simplified): luma-based edge detection + subpixel AA.
// Returns the FXAA-smoothed color at this fragment.
vec3 fxaa(sampler2D tex, vec2 uv, vec2 texel_size) {
  const float kLumaThreshold = 0.5 / 12.0;

  // Sample 3x3 luma.
  vec3 c = texture(tex, uv).rgb;
  float luma_m = dot(c, vec3(0.299, 0.587, 0.114));
  float luma_n  = dot(texture(tex, uv + vec2( 0, -texel_size.y)).rgb, vec3(0.299, 0.587, 0.114));
  float luma_s  = dot(texture(tex, uv + vec2( 0,  texel_size.y)).rgb, vec3(0.299, 0.587, 0.114));
  float luma_w  = dot(texture(tex, uv + vec2(-texel_size.x, 0)).rgb, vec3(0.299, 0.587, 0.114));
  float luma_e  = dot(texture(tex, uv + vec2( texel_size.x, 0)).rgb, vec3(0.299, 0.587, 0.114));
  float luma_nw = dot(texture(tex, uv + vec2(-texel_size.x, -texel_size.y)).rgb, vec3(0.299, 0.587, 0.114));
  float luma_ne = dot(texture(tex, uv + vec2( texel_size.x, -texel_size.y)).rgb, vec3(0.299, 0.587, 0.114));
  float luma_sw = dot(texture(tex, uv + vec2(-texel_size.x,  texel_size.y)).rgb, vec3(0.299, 0.587, 0.114));
  float luma_se = dot(texture(tex, uv + vec2( texel_size.x,  texel_size.y)).rgb, vec3(0.299, 0.587, 0.114));

  float luma_min = min(luma_m, min(min(luma_n, luma_s), min(luma_w, luma_e)));
  float luma_max = max(luma_m, max(max(luma_n, luma_s), max(luma_w, luma_e)));
  float luma_range = luma_max - luma_min;

  // Skip if contrast is below threshold.
  if (luma_range < max(kLumaThreshold, luma_max * 0.0833)) {
    return c;
  }

  // Detect edge direction.
  float edge_h = abs(luma_n + luma_s - 2.0 * luma_m) * 2.0 +
                 abs(luma_nw + luma_sw - 2.0 * luma_w) +
                 abs(luma_ne + luma_se - 2.0 * luma_e);
  float edge_v = abs(luma_w + luma_e - 2.0 * luma_m) * 2.0 +
                 abs(luma_nw + luma_ne - 2.0 * luma_n) +
                 abs(luma_sw + luma_se - 2.0 * luma_s);
  bool is_horizontal = edge_h >= edge_v;

  float step_len = is_horizontal ? texel_size.y : texel_size.x;
  float luma_pos = is_horizontal ? luma_n : luma_w;
  float luma_neg = is_horizontal ? luma_s : luma_e;

  float gradient_pos = abs(luma_pos - luma_m);
  float gradient_neg = abs(luma_neg - luma_m);

  vec2 step_dir;
  if (is_horizontal) {
    step_dir = vec2(0.0, (gradient_pos >= gradient_neg) ? -texel_size.y : texel_size.y);
  } else {
    step_dir = vec2((gradient_pos >= gradient_neg) ? -texel_size.x : texel_size.x, 0.0);
  }

  vec2 offset = step_dir * 0.5;
  vec3 sample_pos = texture(tex, uv + offset).rgb;
  vec3 sample_neg = texture(tex, uv - offset).rgb;
  vec3 result = (sample_pos + sample_neg) * 0.5;

  // Simple blend: mix original with smooth.
  return mix(c, result, 0.5);
}

void main() {
  vec2 texel_size = 1.0 / vec2(textureSize(hdr_image, 0));

  // FXAA first (operates on linear HDR).
  vec3 hdr = fxaa(hdr_image, v_uv, texel_size);

  // ACES filmic tonemapping.
  vec3 mapped = aces_tonemap(hdr);

  // Linear -> sRGB gamma.
  mapped = pow(mapped, vec3(1.0 / 2.2));

  out_color = vec4(mapped, 1.0);
}
