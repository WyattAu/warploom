#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

// pbr_rt_full.frag — the composed full-lighting PBR stage with HARD
// RAY-QUERY SHADOWS: identical to pbr_full.frag except the set-4 shadow
// map (UBO + compare-sampled depth) is replaced by the scene TLAS
// (accelerationStructureEXT, set 4 binding 0) and the PCF kernel by a
// single occlusion rayQuery toward the light. Pipeline layout shape is
// unchanged (6 sets), so the viewport swaps one layout entry.
// Original:
// pbr_ibl_shadow.frag (Cook-Torrance + IBL split-sum + PCF shadow map) with
// the IBL cube/LUT set moved from 3 to 5 so the same pipeline layout can
// also bind the GPU-skinning bone SSBO (set 3) from skinned_scene.vert.
// Set map: 0 mesh SSBO | 1 bindless textures | 2 materials | 3 bones (vert)
//          | 4 scene TLAS           | 5 IBL (prefiltered cube, irradiance
//          cube, BRDF LUT).


// pbr_ibl_shadow.frag — identical to pbr_ibl.frag (Cook-Torrance + IBL)
// plus shadow-map PCF from set 4. The direct-lighting Lo term is multiplied
// by the shadow factor before being added to the ambient, so shadowed
// surfaces receive only the IBL ambient contribution.

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

layout(set = 5, binding = 0) uniform samplerCube env_prefiltered;
layout(set = 5, binding = 1) uniform samplerCube env_irradiance;
layout(set = 5, binding = 2) uniform sampler2D env_brdf_lut;

// Set 4: shadow resources — UBO with light VP (binding 0) + shadow depth (binding 1).
// Set 4: the scene TLAS. All instances carry the OPAQUE flag, so candidate
// intersections commit automatically and the canonical drain loop suffices.
layout(set = 4, binding = 0) uniform accelerationStructureEXT scene_as;

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
  float denom = ndotv * (1.0 - k) + k;
  return ndotv / denom;
}

float G_Smith(float ndotv, float ndotl, float roughness) {
  return G_SchlickGGX(ndotv, roughness) * G_SchlickGGX(ndotl, roughness);
}

vec3 F_Schlick(float cos_theta, vec3 f0) {
  float t  = 1.0 - cos_theta;
  float t5 = t * t * t * t * t;
  return f0 + (1.0 - f0) * t5;
}

vec3 F_SchlickRoughness(float cos_theta, vec3 f0, float roughness) {
  return f0 + (max(vec3(1.0 - roughness), f0) - f0) *
                  pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

vec3 perturbNormal(vec3 n, vec2 uv, sampler2D normal_map) {
  vec3 t0 = dFdx(v_world_pos);
  vec3 t1 = dFdy(v_world_pos);
  vec2 s0 = dFdx(uv);
  vec2 s1 = dFdy(uv);
  vec3 t = normalize(t0 * s1.y - t1 * s0.y);
  vec3 b = -normalize(cross(n, t));
  mat3 tbn = mat3(t, b, n);
  vec3 sample_normal = texture(normal_map, uv).rgb * 2.0 - 1.0;
  return normalize(tbn * sample_normal);
}

// Hard shadow factor via one ray query toward the light. Returns 0.0
// (fully shadowed) to 1.0 (fully lit). The ray origin is offset along the
// geometric normal (the proven pbr_rt_shadow.frag recipe): offsetting along
// the light instead re-enters the surface at grazing angles and self-shadows
// every grazing texel.
float rtShadow(vec3 world_pos, vec3 n) {
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, scene_as, gl_RayFlagsOpaqueEXT, 0xFFu,
                        world_pos + n * 0.01, 0.001,
                        kLightDir, 1.0e30);
  while (rayQueryProceedEXT(rq)) {}
  return rayQueryGetIntersectionTypeEXT(rq, true) ==
                 gl_RayQueryCommittedIntersectionNoneEXT
             ? 1.0
             : 0.0;
}

void main() {
  const PbrMaterial mat = mat_buf.materials[pc.material_index];

  const vec3 albedo_tex =
      texture(textures[nonuniformEXT(mat.albedo_index)], v_uv).rgb;
  const vec3 albedo = v_color * mat.base_color_factor.rgb * albedo_tex;
  const float alpha = mat.base_color_factor.a;

  float metallic  = mat.metallic_factor;
  float roughness = mat.roughness_factor;
  if ((mat.flags & kHasMR) != 0u) {
    const vec3 mr = texture(textures[nonuniformEXT(mat.metallic_roughness_index)],
                            v_uv).rgb;
    roughness *= mr.g;
    metallic  *= mr.b;
  }
  roughness = clamp(roughness, 0.04, 1.0);
  metallic  = clamp(metallic, 0.0, 1.0);

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
  const float d = D_GGX(ndoth, roughness);
  const float g = G_Smith(ndotv, ndotl, roughness);
  const vec3 f = F_Schlick(hdotv, f0);
  const vec3 specular = (d * g * f) / (4.0 * ndotv * ndotl + 0.0001);

  const vec3 kd = (1.0 - f) * (1.0 - metallic);
  const vec3 diffuse = kd * albedo / 3.14159265;

  // Hard shadows: one occlusion ray toward the light against the scene TLAS.
  const float shadow = rtShadow(v_world_pos, n);

  const vec3 lo = (diffuse + specular) * kLightColor * ndotl * shadow;

  // IBL ambient (unchanged from pbr_ibl.frag).
  float ao = 1.0;
  if ((mat.flags & kHasAO) != 0u) {
    ao = mix(1.0, texture(textures[nonuniformEXT(mat.ao_index)], v_uv).r,
             mat.ao_strength);
  }
  const vec3 r = reflect(-v, n);
  const vec3 f_ambient = F_SchlickRoughness(ndotv, f0, roughness);
  const vec3 kd_ambient = (1.0 - f_ambient) * (1.0 - metallic);

  const vec3 irradiance = texture(env_irradiance, n).rgb;
  const vec3 ibl_diffuse = irradiance * albedo;

  const int mip_count = textureQueryLevels(env_prefiltered);
  const float mip = roughness * float(max(mip_count - 1, 0));
  const vec3 prefiltered = textureLod(env_prefiltered, r, mip).rgb;
  const vec2 brdf = texture(env_brdf_lut, vec2(ndotv, roughness)).xy;
  const vec3 ibl_specular = prefiltered * (f0 * brdf.x + brdf.y);

  const vec3 ambient = (kd_ambient * ibl_diffuse + ibl_specular) * ao;

  vec3 emissive = mat.emissive_factor;
  if ((mat.flags & kHasEmissive) != 0u) {
    emissive *= texture(textures[nonuniformEXT(mat.emissive_index)], v_uv).rgb;
  }

  vec3 color = ambient + lo + emissive;
  color = color / (color + vec3(1.0));
  color = pow(color, vec3(1.0 / 2.2));
  out_color = vec4(color, alpha);
}
