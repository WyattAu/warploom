#version 450
#extension GL_EXT_nonuniform_qualifier : require

// pbr_shadow.frag — PBR + shadow mapping WITHOUT IBL. Same Cook-Torrance
// direct lighting as pbr_scene.frag, with PCF shadow from set 4.
// Sets: 0=mesh, 1=textures, 2=material, 4=shadow (no set 3 = IBL).

layout(location = 0) in vec3 v_color;
layout(location = 1) in vec3 v_world_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_world_pos;

layout(location = 0) out vec4 out_color;

layout(set = 1, binding = 0) uniform sampler2D textures[];
struct PbrMaterial {
  vec4 base_color_factor;
  vec3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float ao_strength;
  uint flags;
  uint albedo_index;
  uint normal_index;
  uint metallic_roughness_index;
  uint emissive_index;
  uint ao_index;
};

layout(set = 2, binding = 0, std430) readonly buffer MaterialBuffer {
  PbrMaterial materials[];
} mat_buf;

layout(set = 3, binding = 0) uniform ShadowUBO {
  mat4 light_vp;
} shadow_ubo;
layout(set = 3, binding = 1) uniform sampler2DShadow shadow_map;

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
  vec4 camera_position;
  uint material_index;
  uint pad0;
  uint pad1;
  uint pad2;
} pc;

const uint kHasAlbedo   = 0x1u;
const uint kHasNormal   = 0x2u;
const uint kHasMR       = 0x4u;
const uint kHasEmissive = 0x8u;
const uint kHasAO       = 0x10u;

const vec3 kLightDir   = normalize(vec3(0.3, 0.65, 0.7));
const vec3 kLightColor = vec3(1.0);

float D_GGX(float ndoth, float roughness) {
  float a  = roughness * roughness;
  float a2 = a * a;
  float d  = ndoth * ndoth * (a2 - 1.0) + 1.0;
  return a2 / (3.14159265 * d * d + 0.0001);
}

float G_SchlickGGX(float ndotv, float roughness) {
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;
  return ndotv / (ndotv * (1.0 - k) + k);
}

float G_Smith(float ndotv, float ndotl, float roughness) {
  return G_SchlickGGX(ndotv, roughness) * G_SchlickGGX(ndotl, roughness);
}

vec3 F_Schlick(float cos_theta, vec3 f0) {
  float t5 = pow(1.0 - cos_theta, 5.0);
  return f0 + (1.0 - f0) * t5;
}

vec3 perturbNormal(vec3 n, vec2 uv, sampler2D normal_map) {
  vec3 t0 = dFdx(v_world_pos);
  vec3 t1 = dFdy(v_world_pos);
  vec2 s0 = dFdx(uv);
  vec2 s1 = dFdy(uv);
  vec3 t = normalize(t0 * s1.y - t1 * s0.y);
  vec3 b = -normalize(cross(n, t));
  mat3 tbn = mat3(t, b, n);
  return normalize(tbn * (texture(normal_map, uv).rgb * 2.0 - 1.0));
}

float shadowPCF(vec3 coord) {
  if (coord.x < 0.0 || coord.x > 1.0 ||
      coord.y < 0.0 || coord.y > 1.0 ||
      coord.z < 0.0 || coord.z > 1.0) return 1.0;
  return texture(shadow_map, coord);
}

void main() {
  const PbrMaterial mat = mat_buf.materials[pc.material_index];
  const vec3 albedo = v_color * mat.base_color_factor.rgb *
      texture(textures[nonuniformEXT(mat.albedo_index)], v_uv).rgb;
  const float alpha = mat.base_color_factor.a;

  float metallic = mat.metallic_factor;
  float roughness = mat.roughness_factor;
  if ((mat.flags & kHasMR) != 0u) {
    const vec3 mr = texture(textures[nonuniformEXT(mat.metallic_roughness_index)], v_uv).rgb;
    roughness *= mr.g;
    metallic *= mr.b;
  }
  roughness = clamp(roughness, 0.04, 1.0);
  metallic = clamp(metallic, 0.0, 1.0);

  vec3 n = normalize(v_world_normal);
  if ((mat.flags & kHasNormal) != 0u) {
    n = perturbNormal(n, v_uv, textures[nonuniformEXT(mat.normal_index)]);
  }

  const vec3 v = normalize(pc.camera_position.xyz - v_world_pos);
  const vec3 l = kLightDir;
  const vec3 h = normalize(v + l);
  const float ndotl = max(dot(n, l), 0.0);
  const float ndotv = max(dot(n, v), 0.001);
  const float ndoth = max(dot(n, h), 0.0);
  const float hdotv = max(dot(h, v), 0.0);

  const vec3 f0 = mix(vec3(0.04), albedo, metallic);
  const vec3 specular = (D_GGX(ndoth, roughness) * G_Smith(ndotv, ndotl, roughness) *
      F_Schlick(hdotv, f0)) / (4.0 * ndotv * ndotl + 0.0001);
  const vec3 kd = (1.0 - F_Schlick(hdotv, f0)) * (1.0 - metallic);
  const vec3 lo = (kd * albedo / 3.14159265 + specular) * kLightColor * ndotl;

  // Shadow lookup.
  vec4 lightClipPos = shadow_ubo.light_vp * vec4(v_world_pos, 1.0);
  vec3 shadowCoord = lightClipPos.xyz / lightClipPos.w * 0.5 + 0.5;
  const float shadow = shadowPCF(shadowCoord);

  // Simple ambient + emissive.
  const vec3 ambient = albedo * vec3(0.03);
  vec3 emissive = mat.emissive_factor;
  if ((mat.flags & kHasEmissive) != 0u) {
    emissive *= texture(textures[nonuniformEXT(mat.emissive_index)], v_uv).rgb;
  }
  vec3 color = ambient + lo * shadow + emissive;
  color = color / (color + vec3(1.0));
  color = pow(color, vec3(1.0 / 2.2));
  out_color = vec4(color, alpha);
}
