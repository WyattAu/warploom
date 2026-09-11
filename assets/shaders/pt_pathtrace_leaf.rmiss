#version 460
#extension GL_EXT_ray_tracing : require

// pt_pathtrace_leaf.rmiss — bounce-level miss (SBT miss group 1). The
// bounce-level closest-hit decodes w < 0 as "sky contribution".

layout(location = 1) rayPayloadInEXT vec4 payload;

void main() {
  payload = vec4(0.0, 0.0, 0.0, -1.0);
}
