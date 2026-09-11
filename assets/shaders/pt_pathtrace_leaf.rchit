#version 460
#extension GL_EXT_ray_tracing : require

// pt_pathtrace_leaf.rchit — leaf-level closest hit (SBT hit group 1).
// Terminal shading: flat instance color, no further trace. w = 1.0 marks
// "hit" for the bounce-level decode (w < 0 is the miss sentinel).

struct InstanceData {
  vec4 color;
  vec4 center;
};

layout(set = 0, binding = 2, std430) readonly buffer InstanceColors {
  InstanceData instances[];
} scene_instances;

layout(location = 1) rayPayloadInEXT vec4 payload;

void main() {
  const vec3 color = scene_instances.instances[gl_InstanceCustomIndexEXT].color.rgb;
  payload = vec4(color, 1.0);
}
