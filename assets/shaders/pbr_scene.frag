#version 450
#extension GL_EXT_nonuniform_qualifier : require

// PBR fragment stage implementing the Cook-Torrance microfacet BRDF with the
// glTF 2.0 metallic-roughness workflow:
//   - Base color: vertex color x base_color_factor x albedo texture
//   - Metallic / roughness: factors, optionally modulated per-texel by an
//     MR texture (G = roughness, B = metallic)
//   - Normal mapping (tangent space via UV screen-space derivatives)
//   - Ambient occlusion (multiplies the ambient term)
//   - Emissive (additive, unlit, factor x optional texture)
//
// Material parameters and the bindless indices of its optional maps arrive in
// one 64-byte SSBO slot (set 2) addressed by pc.material_index; texture
// sampling goes through the bindless set-1 array (element 0 = opaque white
// fallback). IBL (split-sum approximation) is intentionally absent from this
// first pass: a directional key light + small ambient keeps the BRDF
// self-contained and provable.  IBL can be layered in later by adding an
// environment cubemap + BRDF LUT to set 3.

layout(location = 0) in vec3 v_color;
layout(location = 1) in vec3 v_world_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_world_pos;

layout(location = 0) out vec4 out_color;

// Bindless texture array (set 1).  Every material texture is an element:
//   [0] = opaque-white fallback
//   [1..N] = albedo, normal, metallic-roughness, emissive, AO textures
// Element indices are read from the material's SSBO slot.
layout(set = 1, binding = 0) uniform sampler2D textures[];

// Material SSBO (set 2), one PbrMaterial slot per material.  Byte layout
// matches PbrMaterialData on the CPU (std430, exactly 64 bytes/slot):
//   vec4 base_color_factor @ 0 | vec3 emissive_factor @ 16 |
//   float metallic @ 28 | float roughness @ 32 | float ao_strength @ 36 |
//   uint flags @ 40 | five bindless indices @ 44..60
struct PbrMaterial {
  vec4 base_color_factor;   // 16  (offset 0)
  vec3 emissive_factor;     // 12  (offset 16)
  float metallic_factor;    // 4   (offset 28)
  float roughness_factor;   // 4   (offset 32)
  float ao_strength;        // 4   (offset 36, 0 = no AO map, 1 = full)
  uint flags;               // 4   (offset 40)
  uint albedo_index;        // 4   (offset 44; 0 = white fallback)
  uint normal_index;        // 4   (offset 48; 0 = geometric normal)
  uint metallic_roughness_index;  // 4 (offset 52; 0 = factors only)
  uint emissive_index;      // 4   (offset 56; 0 = factors only)
  uint ao_index;            // 4   (offset 60; 0 = no AO map)
};  // stride 64

layout(set = 2, binding = 0, std430) readonly buffer MaterialBuffer {
  PbrMaterial materials[];
} mat_buf;

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
  vec4 camera_position;
  uint material_index;
  uint pad0;
  uint pad1;
  uint pad2;
} pc;

// Flag bits in PbrMaterialData::flags (see PbrMaterialFlags on the CPU).
const uint kHasAlbedo   = 0x1u;
const uint kHasNormal   = 0x2u;
const uint kHasMR       = 0x4u;
const uint kHasEmissive = 0x8u;
const uint kHasAO       = 0x10u;

// Directional key light, fixed in world space for deterministic shading.
const vec3 kLightDir   = normalize(vec3(0.3, 0.65, 0.7));
const vec3 kLightColor = vec3(1.0);
const vec3 kAmbient    = vec3(0.03);  // small ambient keeps PBR honest

// ---------------------------------------------------------------------------
// Cook-Torrance helpers: GGX NDF, Smith geometry, Schlick Fresnel.
// ---------------------------------------------------------------------------
float D_GGX(float NdotH, float roughness) {
  float a  = roughness * roughness;
  float a2 = a * a;
  float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
  return a2 / (3.14159265 * d * d + 0.0001);
}

float G_SchlickGGX(float NdotV, float roughness) {
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;  // direct-lighting remap (Disney / Epic)
  float denom = NdotV * (1.0 - k) + k;
  return NdotV / denom;
}

float G_Smith(float NdotV, float NdotL, float roughness) {
  return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}

vec3 F_Schlick(float cosTheta, vec3 F0) {
  float t  = 1.0 - cosTheta;
  float t5 = t * t * t * t * t;
  return F0 + (1.0 - F0) * t5;
}

// Tangent-space normal mapping via screen-space UV/world-position derivatives.
vec3 perturbNormal(vec3 N, vec2 uv, sampler2D normal_map) {
  vec3 t0 = dFdx(v_world_pos);
  vec3 t1 = dFdy(v_world_pos);
  vec2 s0 = dFdx(uv);
  vec2 s1 = dFdy(uv);
  vec3 T = normalize(t0 * s1.y - t1 * s0.y);
  vec3 B = -normalize(cross(N, T));
  mat3 TBN = mat3(T, B, N);
  vec3 n_sample = texture(normal_map, uv).rgb;
  n_sample = n_sample * 2.0 - 1.0;  // [0,1] -> [-1,1]
  return normalize(TBN * n_sample);
}

// ---------------------------------------------------------------------------
void main() {
  const PbrMaterial mat = mat_buf.materials[pc.material_index];

  // --- Base color: vertex color x factor x albedo map (0 = white fallback) --
  const vec3 albedo_tex =
      texture(textures[nonuniformEXT(mat.albedo_index)], v_uv).rgb;
  const vec3 albedo = v_color * mat.base_color_factor.rgb * albedo_tex;
  const float alpha = mat.base_color_factor.a;

  // --- Metallic / roughness ---
  float metallic  = mat.metallic_factor;
  float roughness = mat.roughness_factor;
  if ((mat.flags & kHasMR) != 0u) {
    const vec3 mr = texture(textures[nonuniformEXT(mat.metallic_roughness_index)],
                            v_uv).rgb;
    roughness *= mr.g;  // glTF MR texture: G = roughness, B = metallic
    metallic  *= mr.b;
  }
  roughness = clamp(roughness, 0.04, 1.0);  // avoid the specular singularity
  metallic  = clamp(metallic, 0.0, 1.0);

  // --- Normal ---
  vec3 N = normalize(v_world_normal);
  if ((mat.flags & kHasNormal) != 0u) {
    N = perturbNormal(N, v_uv, textures[nonuniformEXT(mat.normal_index)]);
  }

  // --- View vector: surface -> camera (world space) ---
  const vec3 V = normalize(pc.camera_position.xyz - v_world_pos);
  const vec3 L = kLightDir;
  const vec3 H = normalize(V + L);

  const float NdotL = max(dot(N, L), 0.0);
  const float NdotV = max(dot(N, V), 0.001);
  const float NdotH = max(dot(N, H), 0.0);
  const float HdotV = max(dot(H, V), 0.0);

  // Fresnel F0: dielectrics 0.04, metals tint the reflection with albedo.
  const vec3 F0 = mix(vec3(0.04), albedo, metallic);
  const float D = D_GGX(NdotH, roughness);
  const float G = G_Smith(NdotV, NdotL, roughness);
  const vec3 F  = F_Schlick(HdotV, F0);
  const vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

  // Energy-conserving diffuse: metals have no diffuse term.
  const vec3 kD = (1.0 - F) * (1.0 - metallic);
  const vec3 diffuse = kD * albedo / 3.14159265;

  const vec3 Lo = (diffuse + specular) * kLightColor * NdotL;

  // --- Ambient + AO ---
  float ao = 1.0;
  if ((mat.flags & kHasAO) != 0u) {
    ao = mix(1.0, texture(textures[nonuniformEXT(mat.ao_index)], v_uv).r,
             mat.ao_strength);
  }
  const vec3 ambient = kAmbient * albedo * ao;

  // --- Emissive (additive, unlit) ---
  vec3 emissive = mat.emissive_factor;
  if ((mat.flags & kHasEmissive) != 0u) {
    emissive *= texture(textures[nonuniformEXT(mat.emissive_index)], v_uv).rgb;
  }

  vec3 color = ambient + Lo + emissive;

  // Reinhard tone map + sRGB encode for the UNORM target.
  color = color / (color + vec3(1.0));
  color = pow(color, vec3(1.0 / 2.2));

  out_color = vec4(color, alpha);
}
