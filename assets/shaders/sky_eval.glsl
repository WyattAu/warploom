// sky_eval.glsl — the analytic sky as a pure function, shared by the
// raster background pass (sky.frag) and the path tracer miss branch
// (pt_real.rgen). One definition on purpose: two copies of this math is
// how the BLAS geometry drifted, and a sky that disagrees with itself is
// a parity claim no test can pin. Included via GL_GOOGLE_include_directive
// (both glslc and glslangValidator accept it); sky_eval.glsl carries no
// stage extension so the shader build never compiles it standalone.
//
// Convention, documented because the model needs it: the origin MUST be
// surface-anchored (on/above the shell [EARTH_RADIUS, ATMO_RADIUS], e.g.
// (0, EARTH_RADIUS + 100, 0)). A unit-scale scene origin sits deep inside
// the planet, where every direction has enormous optical depth and the sky
// goes black. The background depends on direction only once anchored.

const float SKY_PI = 3.14159265358979323846;

// Planet fixed at origin; the atmosphere shell spans [Re, Ra]. Unit-scale
// scenes never reach these radii, hence the surface-anchor convention.
const float SKY_EARTH_RADIUS = 6360e3;
const float SKY_ATMO_RADIUS = 6420e3;

// Rayleigh phase: forward/backward symmetric lobes.
float sky_rayleigh_phase(float cos_theta) {
  return 3.0 / (16.0 * SKY_PI) * (1.0 + cos_theta * cos_theta);
}

// Henyey-Greenstein phase for Mie scattering.
float sky_mie_phase(float cos_theta, float g) {
  const float g2 = g * g;
  const float num = 3.0 * (1.0 - g2) * (1.0 + cos_theta * cos_theta);
  const float den = 8.0 * SKY_PI * (2.0 + g2) *
                    pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5);
  return num / den;
}

// Density at point p: exponential falloff with altitude over the shell.
float sky_density(vec3 p) {
  const float h = max(length(p) - SKY_EARTH_RADIUS, 0.0);
  return exp(-h * 4.0 / (SKY_ATMO_RADIUS - SKY_EARTH_RADIUS));
}

// March from origin along dir inside [0, t_far), accumulating density.
float sky_march_density(vec3 origin, vec3 dir, float t_far, int steps) {
  const float dt = t_far / float(steps);
  float sum = 0.0;
  for (int i = 0; i < steps; ++i) {
    sum += sky_density(origin + dir * ((float(i) + 0.5) * dt)) * dt;
  }
  return sum;
}

// Ambient floor keeps the night side from going fully black (the sky tests
// assert on relative brightness, not absolute physical values).
float sky_sun_intensity(float mu) { return 20.0; }

// In-scattered radiance along (ro, rd): single-scattering Preetham-style,
// 16 view steps x 8 sun steps, plus the sun disc. All inputs explicit so
// the raster UBO path and the PT params-buffer path call one definition.
vec3 sky_radiance(vec3 ro, vec3 rd, vec4 sun_direction, vec4 rayleigh,
                  vec4 mie, vec4 misc) {
  // Intersect the view ray with the atmosphere shell.
  const float b = dot(ro, rd);
  const float c = dot(ro, ro) - SKY_ATMO_RADIUS * SKY_ATMO_RADIUS;
  const float disc = b * b - c;
  if (disc < 0.0) {
    return vec3(0.0);
  }
  float t_far = -b + sqrt(disc);
  // Stop at the planet if the ray hits the ground.
  const float ground_disc =
      b * b - (dot(ro, ro) - SKY_EARTH_RADIUS * SKY_EARTH_RADIUS);
  if (ground_disc >= 0.0) {
    const float t_ground = -b - sqrt(ground_disc);
    if (t_ground > 0.0) t_far = min(t_far, t_ground);
  }

  const int view_steps = 16;
  const float t_step = t_far / float(view_steps);

  const vec3 beta_r = rayleigh.rgb;
  const float beta_m = mie.x;
  const float g = mie.y;
  const float mu = dot(rd, normalize(sun_direction.xyz));

  vec3 total_r = vec3(0.0);
  vec3 total_m = vec3(0.0);
  float od_view = 0.0;  // accumulated optical depth along the view ray

  for (int i = 0; i < view_steps; ++i) {
    const vec3 p = ro + rd * ((float(i) + 0.5) * t_step);
    const float d = sky_density(p) * t_step;
    od_view += d;

    // Light optical depth from this sample toward the sun (8 steps).
    const float od_sun = sky_march_density(p, normalize(sun_direction.xyz),
                                           2e6, 8);
    // Transmittance camera->sample->sun (Beer's law, per wavelength for R).
    const vec3 tau = beta_r * (od_view + od_sun) +
                     vec3(beta_m * 1.1) * (od_view + od_sun);
    const vec3 transmittance = exp(-tau);

    total_r += transmittance * d;
    total_m += transmittance * d;
  }

  // In-scattered radiance.
  const vec3 color = (total_r * beta_r * sky_rayleigh_phase(mu) +
                      total_m * beta_m * sky_mie_phase(mu, g)) *
                     sky_sun_intensity(mu);

  // Sun disc: sharp falloff just below the horizon to avoid aliasing.
  const float sun_size = 0.9999;
  const float sun_disc = smoothstep(sun_size - 0.0002, sun_size, mu) * 2000.0;

  return color * misc.x + sun_disc * misc.x;
}
