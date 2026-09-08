#version 450

// sky.frag — analytic Preetham-style atmospheric scattering. Given the view
// ray and a sun direction, integrates Rayleigh (blue sky) + Mie (haze)
// in-scattering over a ray-marched path through a spherical atmosphere, then
// adds a sun disc. Single-scattering only; parameters come from a UBO so the
// test can sweep sun elevation and compare horizon vs zenith brightness.

layout(set = 0, binding = 0, std140) uniform SkyParams {
  vec4 sun_direction;  // xyz = normalized dir toward sun, w unused
  vec4 rayleigh;       // rgb = Rayleigh scattering coefficient, a unused
  vec4 mie;            // x = Mie scattering coeff, y = g (asymmetry), z = turbidity, w unused
  vec4 misc;           // x = exposure, yzw unused
} sky;

layout(push_constant) uniform Push {
  vec4 camera_position;  // xyz = world-space eye, w unused
} cam;

layout(location = 0) in vec3 v_ray_dir;
layout(location = 1) in vec3 v_world_pos;
layout(location = 0) out vec4 out_color;

const float PI = 3.14159265358979323846;

  // Ambient floor keeps the night side from going fully black (the tests
// assert on relative brightness, not absolute physical values).
float sun_intensity(float mu) { return 20.0; }


// Planet fixed at origin; camera sits on/above its surface at radius
// (earth_radius + eye_height). Kept simple: everything is measured relative
// to a unit-scale world where the atmosphere shell spans [Re, Ra].
const float EARTH_RADIUS = 6360e3;
const float ATMO_RADIUS = 6420e3;

// Rayleigh phase: forward/backward symmetric lobes.
float rayleigh_phase(float cos_theta) {
  return 3.0 / (16.0 * PI) * (1.0 + cos_theta * cos_theta);
}

// Henyey-Greenstein phase for Mie scattering.
float mie_phase(float cos_theta, float g) {
  const float g2 = g * g;
  const float num = 3.0 * (1.0 - g2) * (1.0 + cos_theta * cos_theta);
  const float den = 8.0 * PI * (2.0 + g2) *
                    pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5);
  return num / den;
}

// Density at point p: exponential falloff with altitude over the shell.
float density(vec3 p) {
  const float h = max(length(p) - EARTH_RADIUS, 0.0);
  return exp(-h * 4.0 / (ATMO_RADIUS - EARTH_RADIUS));
}

// March from origin along dir inside [0, t_far), accumulating density.
float march_density(vec3 origin, vec3 dir, float t_far, int steps) {
  const float dt = t_far / float(steps);
  float sum = 0.0;
  for (int i = 0; i < steps; ++i) {
    sum += density(origin + dir * ((float(i) + 0.5) * dt)) * dt;
  }
  return sum;
}

void main() {
  const vec3 ro = cam.camera_position.xyz;
  const vec3 rd = normalize(v_ray_dir);

  // Intersect the view ray with the atmosphere shell.
  const float b = dot(ro, rd);
  const float c = dot(ro, ro) - ATMO_RADIUS * ATMO_RADIUS;
  const float disc = b * b - c;
  if (disc < 0.0) {
    // Outside the shell (shouldn't happen for a camera at the surface).
    out_color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  float t_far = -b + sqrt(disc);
  // Stop at the planet if the ray hits the ground.
  const float ground_disc = b * b - (dot(ro, ro) - EARTH_RADIUS * EARTH_RADIUS);
  if (ground_disc >= 0.0) {
    const float t_ground = -b - sqrt(ground_disc);
    if (t_ground > 0.0) t_far = min(t_far, t_ground);
  }

  const int view_steps = 16;
  const float t_step = t_far / float(view_steps);

  const vec3 beta_r = sky.rayleigh.rgb;
  const float beta_m = sky.mie.x;
  const float g = sky.mie.y;
  const float mu = dot(rd, normalize(sky.sun_direction.xyz));

  vec3 total_r = vec3(0.0);
  vec3 total_m = vec3(0.0);
  float od_view = 0.0;  // accumulated optical depth along the view ray

  for (int i = 0; i < view_steps; ++i) {
    const vec3 p = ro + rd * ((float(i) + 0.5) * t_step);
    const float d = density(p) * t_step;
    od_view += d;

    // Light optical depth from this sample toward the sun (8 steps).
    const float od_sun = march_density(p, normalize(sky.sun_direction.xyz),
                                       2e6, 8);
    // Transmittance camera->sample->sun (Beer's law, per wavelength for R).
    const vec3 tau = beta_r * (od_view + od_sun) +
                     vec3(beta_m * 1.1) * (od_view + od_sun);
    const vec3 transmittance = exp(-tau);

    total_r += transmittance * d;
    total_m += transmittance * d;
  }

  // In-scattered radiance.
  const vec3 color = (total_r * beta_r * rayleigh_phase(mu) +
                      total_m * beta_m * mie_phase(mu, g)) *
                     sun_intensity(mu);

  // Sun disc: sharp falloff just below the horizon to avoid aliasing.
  const float sun_size = 0.9999;
  const float sun_disc = smoothstep(sun_size - 0.0002, sun_size, mu) * 2000.0;

  out_color = vec4(color * sky.misc.x + sun_disc * sky.misc.x, 1.0);
}
