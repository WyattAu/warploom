#version 460
#extension GL_EXT_ray_tracing : require

// pt_real.rmiss — real-PT miss: flag the sentinel (payload.w = -1); the
// raygen substitutes the frame-constant sky radiance.

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
  payload.w = -1.0;
}
