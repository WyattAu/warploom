#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Material fragment stage: multiplies the (base-color-tinted) vertex color by
// a directional lambert term computed from the interpolated world normal and
// by the material's albedo texture, sampled from the bindless set-1 array at
// the pushed albedo_index. Element 0 of that array is the opaque-white
// fallback, so untextured materials sample it unchanged. The key light
// direction is fixed in world space so shading is deterministic across
// frames; a small ambient term keeps back faces visible.

layout(location = 0) in vec3 v_color;
layout(location = 1) in vec3 v_world_normal;
layout(location = 2) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

// Bindless albedo array (set 1), bound once per scene by record_scene.
layout(set = 1, binding = 0) uniform sampler2D albedos[];

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
  vec4 base_color;
  uint albedo_index;
  uint pad0;
  uint pad1;
  uint pad2;
} pc;

const vec3 kLightDirection = normalize(vec3(0.3, 0.65, 0.7));
// Soft ambient keeps back faces clearly visible while preserving a real
// directional falloff between faces (pixel-classification tests rely on
// channel dominance, so brightness differences must stay within a face).
const float kAmbient = 0.45;

void main() {
  const vec3 n = normalize(v_world_normal);
  const float diffuse = max(dot(n, kLightDirection), 0.0);
  const vec3 albedo = texture(albedos[nonuniformEXT(pc.albedo_index)], v_uv).rgb;
  out_color = vec4(v_color * albedo * (kAmbient + (1.0 - kAmbient) * diffuse), 1.0);
}
