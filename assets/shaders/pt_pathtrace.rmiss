#version 460
#extension GL_EXT_ray_tracing : require

// pt_pathtrace.rmiss — primary-level miss (SBT miss group 0). The raygen
// decodes w < 0 as "sky".

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
  payload = vec4(0.0, 0.0, 0.0, -1.0);
}
