#version 450

// sky.vert — full-screen sky pass. Draws one oversized triangle covering the
// viewport; the fragment stage shades it analytically and writes the far
// depth. The view ray is built from the camera basis (no inverse matrix
// needed): rd = normalize(forward + ndc.x*tan_fov*aspect*right +
// ndc.y*tan_fov*up).

layout(push_constant) uniform Push {
  vec4 camera_position;  // xyz = world-space eye, w = tan_half_fov
  vec4 forward;          // xyz = view forward, w = aspect ratio
  vec4 right;            // xyz = view right, w unused
  vec4 up;               // xyz = view up, w unused
} pc;

layout(location = 0) out vec3 v_ray_dir;
layout(location = 1) out vec3 v_world_pos;

void main() {
  // Triangle covering the viewport in NDC: (-1,-1), (3,-1), (-1,3).
  const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0),
                                  vec2(-1.0, 3.0));
  const vec2 ndc = corners[uint(gl_VertexIndex) % 3u];

  const float tan_fov = pc.camera_position.w;
  const vec3 rd = normalize(
      pc.forward.xyz + ndc.x * tan_fov * pc.forward.w * pc.right.xyz +
      ndc.y * tan_fov * pc.up.xyz);
  v_ray_dir = rd;
  // Far-plane point along the ray (extent is cosmetic; the fragment stage
  // only uses the ray direction and camera origin).
  v_world_pos = pc.camera_position.xyz + rd * 1e6;

  gl_Position = vec4(ndc, 0.999999, 1.0);  // just inside the far plane so a
                                           // LESS depth compare still passes
                                           // against cleared depth 1.0
}
