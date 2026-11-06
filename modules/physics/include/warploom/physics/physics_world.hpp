#pragma once

/**
 * @file physics_world.hpp
 * @brief Deterministic fixed-step physics: semi-implicit Euler integration,
 *        sphere/capsule-plane and sphere/capsule contacts, orientation
 *        integration with contact torque, distance joints, and an opt-in
 *        XPBD velocity-from-position update.
 *
 * This header is the solver only. It knows nothing about the ECS; the bridge
 * that copies body poses into projected transforms lives on EditorSession
 * (spawn_physics_body / tick), which owns the world. An earlier version of
 * this comment claimed the bridge was here and it was not.
 *
 * Determinism contract: `step(dt)` is a pure function of the world state —
 * fixed iteration order (bodies in insertion order, pairs in index order),
 * no wall-clock reads, no allocation, no parallelism. N identical runs
 * produce bit-identical trajectories, which the replay/scenario harness
 * verifies byte-for-byte.
 *
 * Units: meters, seconds, kilograms. Gravity defaults to -9.81 on Y.
 */

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace warploom::physics {

//! Collider shape. SPHERE is the historical shape; CAPSULE adds a segment
//! collider. All new fields default to a zero-length segment with identity
//! orientation and zero spin, which narrows to exactly the sphere path — so
//! existing scenes (shape untouched) integrate bit-identically.
enum class BodyShape : std::uint8_t { Sphere = 0, Capsule = 1 };

struct PhysicsBody {
  float position[3]{0.0f, 0.0f, 0.0f};
  float velocity[3]{0.0f, 0.0f, 0.0f};
  float radius{0.5f};      //!< Sphere radius / capsule cap radius.
  float inverse_mass{1.0f};  //!< 0 = static (immovable).
  float restitution{0.4f};   //!< Bounce coefficient [0, 1].
  BodyShape shape{BodyShape::Sphere};
  float half_height{0.0f};  //!< Capsule half-length (center to cap center).
                            //!< 0 = sphere-equivalent regardless of `shape`.
  float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};  //!< Unit quaternion (x,y,z,w).
  float angular_velocity[3]{0.0f, 0.0f, 0.0f};   //!< Rad/s, world frame.
  //!< Rotation scope (honest limits): orientation integrates spin exactly
  //!< and contact impulses apply (r x J)/I torque, so impacts genuinely
  //!< rotate capsules. What is NOT modeled: spin does not feed back into
  //!< the friction surface velocity (friction stays the historical 0.98
  //!< tangential damp), there is no rolling-resistance or angular damping
  //!< model, and twist uses the transverse inertia (documented approx).
  //!< Zero spin therefore leaves every linear trajectory bit-identical.
};

//! Rotate (x,y,z) by unit quaternion q (x,y,z,w). Deterministic, branchless.
inline void quat_rotate_vector(const float q[4], float x, float y, float z,
                               float& out_x, float& out_y, float& out_z) noexcept {
  // t = 2 * cross(q.xyz, v); out = v + q.w * t + cross(q.xyz, t).
  const float tx = 2.0F * (q[1] * z - q[2] * y);
  const float ty = 2.0F * (q[2] * x - q[0] * z);
  const float tz = 2.0F * (q[0] * y - q[1] * x);
  out_x = x + q[3] * tx + (q[1] * tz - q[2] * ty);
  out_y = y + q[3] * ty + (q[2] * tx - q[0] * tz);
  out_z = z + q[3] * tz + (q[0] * ty - q[1] * tx);
}

//! Bounding radius: sphere radius, plus capsule half-length. The broadphase
//! sizes cells from this, so bounding spheres provably contain every
//! overlapping pair (same conservative argument as the sphere path).
inline float body_bounding_radius(const PhysicsBody& b) noexcept {
  const float extra =
      (b.shape == BodyShape::Capsule && b.half_height > 0.0F) ? b.half_height : 0.0F;
  return b.radius + extra;
}

//! Transverse moment of inertia about the body center (uniform density).
//! Sphere: (2/5) m r^2. Capsule: exact volume-weighted composite of a
//! cylinder (length 2h) plus two hemispheres at +/-h (parallel-axis terms
//! included). Twist about the capsule axis uses the same scalar — a
//! documented approximation (twist inertia is smaller); contacts rarely
//! torque about the axis, and the error direction (under-rotation of pure
//! twist) is stable, never an energy source.
inline float body_inertia(const PhysicsBody& b) noexcept {
  if (b.inverse_mass <= 0.0F) return 0.0F;
  const float m = 1.0F / b.inverse_mass;
  const float r = b.radius;
  if (b.shape != BodyShape::Capsule || b.half_height <= 0.0F) {
    return 0.4F * m * r * r;
  }
  const float h = b.half_height;
  const float vc = 2.0F * h * r * r;         // cylinder volume / pi
  const float vs = 4.0F / 3.0F * r * r * r;  // sphere volume / pi
  const float total = vc + vs;
  if (total <= 0.0F) return 0.4F * m * r * r;
  const float mc = m * vc / total;
  const float ms = m * vs / total;
  const float i_cyl = mc * (3.0F * r * r + 4.0F * h * h) / 12.0F;
  const float i_sph = ms * (0.4F * r * r + h * h);
  return i_cyl + i_sph;
}

//! One resolved contact (exposed for tests/telemetry).
struct ContactEvent {
  std::uint32_t a{0};
  std::uint32_t b{0xFFFFFFFFu};  //!< 0xFFFFFFFF = the world (plane).
  float depth{0.0f};
};

//! Deterministic sphere physics world over a ground plane at y = 0.
class PhysicsWorld final {
 public:
  explicit PhysicsWorld(float gravity = -9.81f) noexcept
      : gravity_{gravity} {}

  [[nodiscard]] std::uint32_t add_body(const PhysicsBody& body) {
    const std::uint32_t id = static_cast<std::uint32_t>(bodies_.size());
    bodies_.push_back(body);
    return id;
  }

  [[nodiscard]] PhysicsBody& body(std::uint32_t id) noexcept {
    return bodies_[id];
  }
  [[nodiscard]] const PhysicsBody& body(std::uint32_t id) const noexcept {
    return bodies_[id];
  }
  [[nodiscard]] std::size_t body_count() const noexcept {
    return bodies_.size();
  }

  [[nodiscard]] const std::vector<ContactEvent>& contacts() const noexcept {
    return contacts_;
  }

  //! Gravity, exposed so a snapshot can carry the world's configuration and not
  //! just its bodies. A resumed replay with the wrong gravity diverges.
  [[nodiscard]] float gravity() const noexcept { return gravity_; }
  //! Snapshot-restore primitive (T4a): the codec rebuilds a world by
  //! clear + re-add in body-id order, so restored ids match the captured
  //! session map. Gravity is configuration, not derived state, so it is
  //! settable for the same reason.
  void set_gravity(float gravity) noexcept { gravity_ = gravity; }
  void clear_bodies() noexcept { bodies_.clear(); }

  //! One fixed step, in phase order:
  //!   1. integrate velocities (gravity) and positions (semi-implicit Euler),
  //!   2. resolve sphere-plane contacts (ground at y = 0, restitution),
  //!   3. resolve sphere-sphere contacts (equal-mass elastic-ish impulse,
  //!      positional correction, index-order pairs, single iteration).
  //! Simulation substeps per step: `step(dt)` runs `substeps_` passes of
  //! (integrate dt/n, detect, resolve). Smaller increments improve stacking
  //! stability independently of the solver-iteration count (which operates
  //! WITHIN each substep). Default 1 = the historical single increment.
  //!
  //! Interaction note: callers may ALSO substep at their level (EditorSession
  //! does, derived from fixed_dt) — the counts compound (session substeps ×
  //! world substeps total increments). That is semantically coherent but must
  //! be chosen deliberately; the default here is 1 so session-driven hosts
  //! see no change unless they opt in.
  void set_substeps(std::uint32_t n) noexcept { substeps_ = n > 0U ? n : 1U; }
  [[nodiscard]] std::uint32_t substeps() const noexcept { return substeps_; }

  //! XPBD contact compliance (Loop 12, staged after Small Steps SIGGRAPH
  //! 2021 / XPBD MIG 2016; exact formulation per trial 638). The
  //! sphere-sphere positional correction is the XPBD update
  //! (C - atilde*lambda)/(w + atilde) with atilde = alpha/h^2 (lambda is
  //! the accumulated separating magnitude, i.e. minus the textbook signed
  //! multiplier — hence the minus here and a C > 0 fixed point),
  //! so alpha > 0 softens contacts with a rest state independent of the
  //! iteration and substep counts, and alpha = 0 is definitionally the
  //! historical stiff projection (pinned by `Loop12ComplianceZeroIsBitIdentical`
  //! and by the Lean proof `compliance_zero_is_stiff`). Default 0:
  //! existing replays reproduce bit-identically unless the caller opts in.
  void set_contact_compliance(float alpha) noexcept {
    contact_compliance_ = alpha >= 0.0F ? alpha : 0.0F;
  }
  [[nodiscard]] float contact_compliance() const noexcept {
    return contact_compliance_;
  }

  //! Restitution velocity threshold (D1 island sleeping, staged after the
  //! research pass). Restitution applies only when the approach speed
  //! exceeds the threshold; slower contacts resolve inelastically (approach
  //! velocity removed, no bounce). Real impacts (m/s) bounce exactly as
  //! before; micro-bounces below the threshold decay to true rest instead
  //! of oscillating forever, which is what lets a settled pile satisfy the
  //! sleep criterion. Default 0 = the historical always-bounce response, so
  //! existing replays reproduce bit-identically unless the caller opts in.
  void set_restitution_threshold(float v) noexcept {
    restitution_threshold_ = v >= 0.0F ? v : 0.0F;
  }
  [[nodiscard]] float restitution_threshold() const noexcept {
    return restitution_threshold_;
  }

  //! Speculative (swept) contacts (D1, R3 gate). When on, detection also
  //! tests the swept segment start->end from the frozen snapshot: a pair
  //! whose path passes within contact distance mid-substep emits a TOI
  //! contact that rewinds the overshoot and kills the approach, so a
  //! fast body stops at the wall instead of tunneling. Default ON:
  //! tunneling is a correctness defect, and the suite verifies the flip
  //! changes nothing except fast crossings (all other scenes are slow
  //! enough that the swept test never fires). set(false) restores the
  //! discrete path textually untouched, for replay archaeology and for
  //! gates that pin the discrete behavior.
  //! Limits (documented, not silent): capsule TOI refines at fixed end
  //! orientations (exact for non-spinning pairs); the rewind is stiff
  //! even under contact compliance; under
  //! velocity-from-position the derived velocities include the TOI landing
  //! by construction. Speculative contacts carry penetration 0, so they
  //! never form island edges (a touched island stays awake — conservative).
  void set_speculative_contacts(bool on) noexcept {
    speculative_contacts_ = on;
  }
  [[nodiscard]] bool speculative_contacts() const noexcept {
    return speculative_contacts_;
  }

  //! Island sleep speed threshold in m/s (D1, Jolt's per-step rebuild
  //! pattern). Each step_single rebuilds islands by union-find over the
  //! frozen contact graph and puts an island to sleep when every dynamic
  //! member's pre-integration speed is below the threshold AND the island
  //! owns at least one contact edge (a slow body in free flight stays
  //! awake). Sleeping members are restored to their pre-integration state
  //! and skipped by the solver that step — a settled pile costs
  //! integration + detection only. No sleep state is stored: islands are
  //! re-derived every step, so waking is automatic (a fast body joining the
  //! island through a new contact fails the rest test) and replay
  //! reproduces every sleep decision bit-identically. Default 0 = never
  //! sleep: the sleep set is always empty and the step is bit-identical to
  //! the historical solver.
  //!
  //! Boundary semantics: a pair with one sleeping endpoint treats the
  //! sleeper as PINNED (infinite mass for this step) — the awake endpoint
  //! takes the full correction and impulse. The alternative (the pair's
  //! usual share) leaves half the approach velocity behind every step and
  //! jitters awake bodies on sleeping ones forever. Small, bounded,
  //! deterministic discontinuity at the sleep/wake boundary.
  void set_sleep_threshold(float v) noexcept {
    sleep_threshold_ = v >= 0.0F ? v : 0.0F;
  }
  [[nodiscard]] float sleep_threshold() const noexcept {
    return sleep_threshold_;
  }
  //! Bodies asleep after the last step_single (derived scratch, exposed for
  //! tests/telemetry — never part of snapshots, which carry bodies only).
  [[nodiscard]] std::uint32_t sleeping_bodies() const noexcept {
    return sleeping_count_;
  }
  [[nodiscard]] bool is_asleep(std::uint32_t id) const noexcept {
    return id < sleeping_flags_.size() && sleeping_flags_[id] != 0U;
  }

  //! Jolt-style record-rewind-replay validator (Loop 12). When enabled,
  //! every step runs the substep sequence TWICE from the same saved state
  //! and compares position fingerprints; a mismatch increments
  //! `determinism_violations()` instead of aborting, so tests pin it
  //! without death-tests. Deterministic-by-construction when the contract
  //! holds: both runs start from identical state and end in identical
  //! state, so enabling the flag perturbs nothing. 2x physics cost while
  //! on; default off, zero release cost. A velocity-only divergence with
  //! identical positions cannot hide for more than one step: positions
  //! integrate velocities every step, so it surfaces in the fingerprint on
  //! the very next checked step.
  void set_determinism_check(bool on) noexcept { determinism_check_ = on; }
  [[nodiscard]] bool determinism_check() const noexcept {
    return determinism_check_;
  }
  [[nodiscard]] std::uint64_t determinism_violations() const noexcept {
    return determinism_violations_;
  }

  //! XPBD velocity-from-position update (research: XPBD MIG 2016, Small
  //! Steps SIGGRAPH 2021 — staged adoption, step 2 of the R2 spec). When
  //! enabled, linear velocities are derived from the corrected positions
  //! (v = (x_new - x_pre) / dt) instead of the separate impulse pass, so
  //! velocities are consistent with the positions the solver actually
  //! produced; restitution is then applied as a bounce on top using the
  //! stored pre-solve approach speeds. Joints are positional-only in
  //! impulse mode but become velocity-consistent under this flag, which is
  //! the reason to prefer it for jointed scenes. Default OFF: the impulse
  //! path runs exactly the historical instruction sequence (pinned by the
  //! full existing suite), so current replays reproduce bit-identically.
  void set_velocity_from_position(bool on) noexcept {
    velocity_from_position_ = on;
  }
  [[nodiscard]] bool velocity_from_position() const noexcept {
    return velocity_from_position_;
  }

  //! A positional distance constraint between two bodies (D1 joints, first
  //! shape: point-to-point rest length). Solved in insertion order inside
  //! every solver pass, alongside contacts; empty by default, so scenes
  //! without joints execute the historical path untouched. Compliance
  //! defaults to 0 (stiff); XPBD compliance scales exactly like contact
  //! compliance. In impulse mode joints move positions only (velocities
  //! stay as the contact solve left them — a documented limit); under
  //! velocity-from-position the derived velocities include joint motion
  //! by construction.
  struct DistanceJoint {
    std::uint32_t a{0};
    std::uint32_t b{0};
    float rest_length{1.0f};
    float compliance{0.0f};
  };
  [[nodiscard]] std::uint32_t add_distance_joint(std::uint32_t a,
                                                 std::uint32_t b,
                                                 float rest_length,
                                                 float compliance = 0.0f) noexcept {
    DistanceJoint j;
    j.a = a;
    j.b = b;
    j.rest_length = rest_length >= 0.0f ? rest_length : 0.0f;
    j.compliance = compliance >= 0.0f ? compliance : 0.0f;
    joints_.push_back(j);
    return static_cast<std::uint32_t>(joints_.size() - 1U);
  }
  void clear_joints() noexcept { joints_.clear(); }
  [[nodiscard]] std::size_t joint_count() const noexcept {
    return joints_.size();
  }
  [[nodiscard]] const DistanceJoint& joint(std::uint32_t id) const noexcept {
    return joints_[id];
  }

  void step(float dt) noexcept {
    if (!determinism_check_) {
      run_substeps(dt);
      return;
    }
    check_saved_ = bodies_;  // member scratch: steady-state no-alloc,
                              // same policy as the other step scratch members
    run_substeps(dt);
    const std::uint64_t fp1 = state_fingerprint();
    bodies_ = check_saved_;
    run_substeps(dt);
    if (state_fingerprint() != fp1) ++determinism_violations_;
  }

  //! One full solve at the given increment (integration + detect + resolve).
  //! Engine detail: step() drives it; kept in the public section only because
  //! the class's private block starts at its data members.
  void step_single(float dt) noexcept {
    contacts_.clear();
    const float g = gravity_;

    // Pre-step state for the island sleeper: sleeping members are restored
    // to exactly this. Analytic undo is NOT bitwise identity in floating
    // point ((p+v·dt)−v·dt != p in general), and a 1-ULP drift per step is
    // a slow sink — so the pose is stored. Reused scratch, no steady alloc.
    pre_step_.resize(bodies_.size());
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
      const PhysicsBody& saved = bodies_[i];
      PreStep& pre = pre_step_[i];
      pre.px = saved.position[0];
      pre.py = saved.position[1];
      pre.pz = saved.position[2];
      pre.vx = saved.velocity[0];
      pre.vy = saved.velocity[1];
      pre.vz = saved.velocity[2];
      pre.qx = saved.orientation[0];
      pre.qy = saved.orientation[1];
      pre.qz = saved.orientation[2];
      pre.qw = saved.orientation[3];
      pre.wx = saved.angular_velocity[0];
      pre.wy = saved.angular_velocity[1];
      pre.wz = saved.angular_velocity[2];
    }

    // --- 1. Integration -------------------------------------------------
    // Unconditional for every dynamic body: sleep is applied by restoring
    // the pre-step state below, never by skipping, so the awake path
    // executes the historical instruction sequence bit-identically.
    // Orientation integrates angular velocity (exact rotation-vector step,
    // renormalized): bodies with zero spin keep the identity quaternion
    // bit-identically, so sphere scenes are untouched.
    for (PhysicsBody& b : bodies_) {
      if (b.inverse_mass <= 0.0F) continue;  // 0 == static
      b.velocity[1] += g * dt;
      b.position[0] += b.velocity[0] * dt;
      b.position[1] += b.velocity[1] * dt;
      b.position[2] += b.velocity[2] * dt;
      const float wx = b.angular_velocity[0];
      const float wy = b.angular_velocity[1];
      const float wz = b.angular_velocity[2];
      const float wmag_sq = wx * wx + wy * wy + wz * wz;
      if (wmag_sq > 0.0F) {
        const float wmag = std::sqrt(wmag_sq);
        const float half = 0.5F * wmag * dt;
        const float s = std::sin(half) / wmag;
        const float dq_x = wx * s;
        const float dq_y = wy * s;
        const float dq_z = wz * s;
        const float dq_w = std::cos(half);
        const float qx = b.orientation[0];
        const float qy = b.orientation[1];
        const float qz = b.orientation[2];
        const float qw = b.orientation[3];
        float nx = dq_w * qx + dq_x * qw + dq_y * qz - dq_z * qy;
        float ny = dq_w * qy - dq_x * qz + dq_y * qw + dq_z * qx;
        float nz = dq_w * qz + dq_x * qy - dq_y * qx + dq_z * qw;
        float nw = dq_w * qw - dq_x * qx - dq_y * qy - dq_z * qz;
        const float nrm =
            std::sqrt(nx * nx + ny * ny + nz * nz + nw * nw);
        if (nrm > 0.0F) {
          nx /= nrm;
          ny /= nrm;
          nz /= nrm;
          nw /= nrm;
        }
        b.orientation[0] = nx;
        b.orientation[1] = ny;
        b.orientation[2] = nz;
        b.orientation[3] = nw;
      }
    }

    // --- 2. Ground plane contacts (y = 0) -------------------------------
    // Grounded membership is recorded for the island builder (phase 3b):
    // post-clamp positions compare == radius, so the contact is no longer
    // re-derivable after this loop. Depth is recorded too, for the
    // penetration gate (dust must not ground an island).
    grounded_.assign(bodies_.size(), 0U);
    grounded_depth_.assign(bodies_.size(), 0.0F);
    grounded_approach_.assign(bodies_.size(), 0.0F);
    for (std::size_t gi = 0; gi < bodies_.size(); ++gi) {
      PhysicsBody& b = bodies_[gi];
      if (b.inverse_mass <= 0.0F) continue;  // 0 == static
      // Support offset: sphere radius, or capsule lowest point
      // (axis * |up.y| plus cap radius). Spheres take the historical path
      // exactly: support == radius, contact at (x, 0, z).
      float support = b.radius;
      float upx = 0.0F, upy = 1.0F, upz = 0.0F;
      if (b.shape == BodyShape::Capsule && b.half_height > 0.0F) {
        quat_rotate_vector(b.orientation, 0.0F, 1.0F, 0.0F, upx, upy, upz);
        float aupx = upy >= 0.0F ? upy : -upy;
        support = aupx * b.half_height + b.radius;
      }
      if (b.position[1] - support < 0.0f) {
        const float depth = support - b.position[1];
        b.position[1] = support;
      // Contact point and lever arm for torque (side effect on spin only:
      // linear updates below are the historical ones, bit-identical).
      // cp=(pos.x, 0, pos.z), so r=(0, -support, 0).
      const float rx = 0.0F;
      const float ry = 0.0F - support;
      const float rz = 0.0F;
        const float inv_m = b.inverse_mass;
        const float mass = inv_m > 0.0F ? 1.0F / inv_m : 0.0F;
        if (!velocity_from_position_) {
          const float old_vx = b.velocity[0];
          const float old_vy = b.velocity[1];
          const float old_vz = b.velocity[2];
          if (b.velocity[1] < 0.0f) {
            // Restitution threshold (opt-in): impacts slower than the
            // threshold resolve inelastically instead of micro-bouncing.
            // Default 0 keeps the historical always-bounce response: vy < 0
            // implies -vy > 0, so the condition is exactly the old branch.
            b.velocity[1] = -b.velocity[1] > restitution_threshold_
                                  ? -b.velocity[1] * b.restitution
                                  : 0.0f;
          }
          // Ground friction damps tangential motion while contacting.
          b.velocity[0] *= 0.98f;
          b.velocity[2] *= 0.98f;
          // Torque from the ground impulse (spin side effect only).
          if (mass > 0.0F) {
            const float jx = mass * (b.velocity[0] - old_vx);
            const float jy = mass * (b.velocity[1] - old_vy);
            const float jz = mass * (b.velocity[2] - old_vz);
            const float inertia = body_inertia(b);
            if (inertia > 0.0F) {
              // dW = (r x J) / I, r = (0, -support, 0).
              b.angular_velocity[0] += (ry * jz - rz * jy) / inertia;
              b.angular_velocity[1] += (rz * jx - rx * jz) / inertia;
              b.angular_velocity[2] += (rx * jy - ry * jx) / inertia;
            }
          }
        } else {
          // VFP mode: positions only here; velocities are derived after the
          // solve. Record the approach speed for the bounce stage.
          grounded_approach_[gi] =
              b.velocity[1] < 0.0F ? -b.velocity[1] : 0.0F;
        }
        contacts_.push_back(ContactEvent{0U, kWorldContact, depth});
        grounded_[gi] = 1U;
        grounded_depth_[gi] = depth;
      }
    }

    // --- 3. Sphere-sphere contacts ---------------------------------------
    // Detection reads ONE snapshot of positions and velocities, so the set of
    // contacts does not depend on the order pairs are visited. The previous
    // form re-read live positions per pair while also mutating them, which
    // made the result depend on visit order AND made it impossible to replace
    // the all-pairs scan with a broadphase: a body pushed into an overlap by an
    // earlier correction would be missed by any candidate list built before the
    // pass. Freezing detection makes the pair set a pure function of the
    // snapshot, which is what lets a spatial grid be exactly conservative.
    //
    // Corrections are still applied in pair order -- that is Gauss-Seidel, and
    // it converges better than a simultaneous solve. What is fixed is *which*
    // pairs get solved, not the order their corrections land in.
    if (bodies_.size() >= 2U) {
      solve_snapshot_.assign(bodies_.size(), SolveSnapshot{});
      for (std::size_t i = 0; i < bodies_.size(); ++i) {
        const PhysicsBody& body = bodies_[i];
        SolveSnapshot& snap = solve_snapshot_[i];
        snap.px = body.position[0];
        snap.py = body.position[1];
        snap.pz = body.position[2];
        snap.vx = body.velocity[0];
        snap.vy = body.velocity[1];
        snap.vz = body.velocity[2];
        snap.radius = body.radius;
        snap.inverse_mass = body.inverse_mass;
        snap.restitution = body.restitution;
        snap.shape = body.shape;
        snap.half_height =
            (body.shape == BodyShape::Capsule && body.half_height > 0.0F)
                ? body.half_height
                : 0.0F;
        snap.qx = body.orientation[0];
        snap.qy = body.orientation[1];
        snap.qz = body.orientation[2];
        snap.qw = body.orientation[3];
        snap.bound = snap.radius + snap.half_height;
      }

      solve_contacts_.clear();
      rebuild_candidate_pairs(dt);
      for (const std::pair<std::uint32_t, std::uint32_t>& pair :
           candidate_pairs_) {
        ContactSolve solve{};
        if (narrowphase_pair(pair.first, pair.second, solve, dt)) {
          solve_contacts_.push_back(solve);
        }
      }
      // Trial 638: exact-XPBD Lagrange multipliers, one per frozen
      // contact, zeroed every substep (XPBD keeps lambda only within a
      // step's iterations). Reused scratch, no steady-state allocation.
      // atilde = alpha / h^2 is the XPBD scaled compliance: the same
      // alpha means the same softness at any substep count or iteration
      // count. alpha = 0 gives atilde = 0 exactly, i.e. the historical
      // stiff projection bit-for-bit (pen + 0*lambda == pen in IEEE).
      solve_lambda_.assign(solve_contacts_.size(), 0.0f);
      const float h_sq = dt * dt;
      const float contact_atilde =
          (contact_compliance_ > 0.0f && h_sq > 0.0f)
              ? contact_compliance_ / h_sq
              : 0.0f;
      joint_lambda_.assign(joints_.size(), 0.0f);

      // Islands are derived from the frozen pair list plus the grounded
      // flags BEFORE any correction lands, so both the positional passes
      // and the impulse loop below read this step's sleep set.
      build_islands();

      // Positional correction: `solver_iterations_` Gauss-Seidel passes over
      // the frozen contact list (the SET of pairs is frozen; the impulse
      // below stays single-shot on the snapshot's relative velocity).
      // Pass 0 applies the snapshot penetration. Passes 1+ re-derive the
      // RESIDUAL overlap from live positions along the frozen normal and
      // skip separated pairs. Re-applying the frozen depth instead is an
      // energy pump: measured, a blinking pair (detected every other step)
      // over-separates 4x per contact step and sustains a 2-step limit
      // cycle at ~0.4 m/s on a sleeping base forever — and the same
      // mechanism launches bodies off pinned endpoints at metres per
      // second. Iterations=1 (the default) executes pass 0 only, so the
      // default path is untouched by this. No sleeper exception is needed:
      // residual corrections vanish as pairs separate, so later passes
      // cannot pump (an earlier pass-0-only guard for sleeper pairs was
      // tried and REMOVED — it starved exactly the pairs that needed
      // re-convergence, sinking the layer above a sleeper by millimetres).
      // Joints are solved inside every pass, before contacts, in insertion
      // order (deterministic). With no joints this loop is empty and the
      // pass is the historical one.
      for (std::uint32_t pass = 0; pass < solver_iterations_; ++pass) {
        // Speculative rewind (pass 0 only): place the pair back at its TOI
        // configuration by removing the relative overshoot split by inverse
        // mass. Passes 1+ use the generic residual path below: after a TOI
        // landing the residual is ~0 along n, so they skip — the same
        // mechanism that settles discrete pairs. Sleep-pinned endpoints
        // contribute 0 mass exactly like the discrete branch.
        if (pass == 0U) {
          for (std::size_t ci = 0; ci < solve_contacts_.size(); ++ci) {
            const ContactSolve& solve = solve_contacts_[ci];
            if (!solve.speculative) continue;
            PhysicsBody& a = bodies_[solve.a];
            PhysicsBody& b = bodies_[solve.b];
            const float inv_a =
                is_asleep(solve.a) ? 0.0f : a.inverse_mass;
            const float inv_b =
                is_asleep(solve.b) ? 0.0f : b.inverse_mass;
            const float inv_eff = inv_a + inv_b;
            if (inv_eff <= 0.0f) continue;  // both pinned
            // W points from TOI to end (overshoot direction); the rewind
            // moves the pair back along it, so a takes +W and b takes -W
            // (relative change -W, center-of-mass preserving).
            const float fa = inv_a / inv_eff;
            const float fb = inv_b / inv_eff;
            a.position[0] += solve.wx * fa;
            a.position[1] += solve.wy * fa;
            a.position[2] += solve.wz * fa;
            b.position[0] -= solve.wx * fb;
            b.position[1] -= solve.wy * fb;
            b.position[2] -= solve.wz * fb;
          }
        }
        for (std::size_t ji = 0; ji < joints_.size(); ++ji) {
          const DistanceJoint& joint = joints_[ji];
          if (joint.a == joint.b || joint.a >= bodies_.size() ||
              joint.b >= bodies_.size()) {
            continue;
          }
          PhysicsBody& ja = bodies_[joint.a];
          PhysicsBody& jb = bodies_[joint.b];
          const float jdx = jb.position[0] - ja.position[0];
          const float jdy = jb.position[1] - ja.position[1];
          const float jdz = jb.position[2] - ja.position[2];
          const float jdist_sq = jdx * jdx + jdy * jdy + jdz * jdz;
          if (jdist_sq <= 1e-16F) continue;  // coincident: no direction
          const float jdist = std::sqrt(jdist_sq);
          const float jnx = jdx / jdist;
          const float jny = jdy / jdist;
          const float jnz = jdz / jdist;
          const float j_inv_a =
              is_asleep(joint.a) ? 0.0f : ja.inverse_mass;
          const float j_inv_b =
              is_asleep(joint.b) ? 0.0f : jb.inverse_mass;
          const float j_inv_eff = j_inv_a + j_inv_b;
          if (j_inv_eff <= 0.0f) continue;  // both pinned
          // Trial 638: exact XPBD joint solve. C = dist - rest, bilateral
          // (a stretched joint pulls back too — unlike contact, no
          // separation skip), atilde = alpha / h^2 like contacts. lambda
          // is kept NONNEGATIVE by convention (accumulated separating
          // magnitude), so the textbook -atilde*lambda term enters with a
          // minus here: jcorr = (C - atilde*lambda)/(w + atilde).
          const float j_atilde =
              (joint.compliance > 0.0f && h_sq > 0.0f)
                  ? joint.compliance / h_sq
                  : 0.0f;
          const float jcorr = ((jdist - joint.rest_length) -
                               j_atilde * joint_lambda_[ji]) /
                              (j_inv_eff + j_atilde);
          joint_lambda_[ji] += jcorr;
          if (j_inv_a > 0.0f) {
            ja.position[0] += jnx * jcorr * j_inv_a;
            ja.position[1] += jny * jcorr * j_inv_a;
            ja.position[2] += jnz * jcorr * j_inv_a;
          }
          if (j_inv_b > 0.0f) {
            jb.position[0] -= jnx * jcorr * j_inv_b;
            jb.position[1] -= jny * jcorr * j_inv_b;
            jb.position[2] -= jnz * jcorr * j_inv_b;
          }
        }
        for (std::size_t ci = 0; ci < solve_contacts_.size(); ++ci) {
          const ContactSolve& solve = solve_contacts_[ci];
          float pen = solve.penetration;
          if (pass > 0U) {
            const PhysicsBody& la = bodies_[solve.a];
            const PhysicsBody& lb = bodies_[solve.b];
            const float dx = lb.position[0] - la.position[0];
            const float dy = lb.position[1] - la.position[1];
            const float dz = lb.position[2] - la.position[2];
            pen = (la.radius + lb.radius) -
                  (dx * solve.nx + dy * solve.ny + dz * solve.nz);
            if (pen <= 0.0f) continue;  // separated: nothing to do
          }
          PhysicsBody& a = bodies_[solve.a];
          PhysicsBody& b = bodies_[solve.b];
          // Positional correction proportional to inverse mass. The
          // compliance term is 0 by default, i.e. the historical stiff
          // projection; XPBD staging (Loop 12) opts in via
          // set_contact_compliance(). Sleeping endpoints count as PINNED
          // (effective inverse mass 0): the awake side takes the full
          // correction. The alternative — the pair's usual half-share —
          // leaves residual overlap AND half the approach velocity behind
          // every step, which jitters the layer above a sleeper forever
          // and nothing ever sleeps (measured: top layers sustained ~1 m/s
          // on a sleeping base). A woken body is corrected normally
          // because it is not in this step's sleep set.
          const float inv_a =
              is_asleep(solve.a) ? 0.0f : a.inverse_mass;
          const float inv_b =
              is_asleep(solve.b) ? 0.0f : b.inverse_mass;
          const float inv_eff = inv_a + inv_b;
          if (inv_eff <= 0.0f) continue;  // both pinned: restore keeps them
          // Trial 638: exact XPBD contact solve (Muller et al., XPBD).
          // Textbook dlambda = (-C - atilde*lambda)/(w + atilde); lambda
          // here is the accumulated SEPARATING magnitude (nonnegative by
          // construction), i.e. minus the textbook signed multiplier, so
          // the atilde term enters with a minus: the fixed point is
          // C = atilde*lambda > 0 (compliant penetration under load), not
          // separation. The separation skip stays: contact is unilateral
          // (C >= 0), so a separated pair applies nothing — unlike a
          // bilateral joint.
          const float corr = (pen - contact_atilde * solve_lambda_[ci]) /
                             (inv_eff + contact_atilde);
          solve_lambda_[ci] += corr;
          if (inv_a > 0.0f) {
            a.position[0] -= solve.nx * corr * inv_a;
            a.position[1] -= solve.ny * corr * inv_a;
            a.position[2] -= solve.nz * corr * inv_a;
          }
          if (inv_b > 0.0f) {
            b.position[0] += solve.nx * corr * inv_b;
            b.position[1] += solve.ny * corr * inv_b;
            b.position[2] += solve.nz * corr * inv_b;
          }
        }
      }
      if (!velocity_from_position_) {
        for (const ContactSolve& solve : solve_contacts_) {
          PhysicsBody& a = bodies_[solve.a];
          PhysicsBody& b = bodies_[solve.b];

          // Impulse along the normal. vel_n comes from the snapshot, so this
          // does not depend on corrections applied earlier in this loop —
          // with one exception: a SLEEPING endpoint contributes its PRE-step
          // (rest) velocity, not its integrated snapshot velocity. The
          // sleeper integrated gravity this step and is restored afterwards,
          // so the snapshot carries one gravity increment that was never
          // real motion; measuring approach against it leaves exactly that
          // increment behind every step (a lone body on a sleeping base
          // hard-fixed-points at -g·dt = -0.082, forever above any sane
          // sleep threshold). The ground clamp already works this way — it
          // sets absolute velocity, never relative — so this makes a pinned
          // base behave exactly like the ground. All-awake pairs use the
          // snapshot for both sides, i.e. the historical computation.
          // Sleeping endpoints are pinned (see above): the impulse is
          // computed against the awake side's mass alone, so an inelastic
          // contact fully removes the approach velocity instead of half of
          // it. Events emit for every detected pair either way, so
          // contacts_ stays a pure function of detection.
          float vel_n = solve.vel_n;
          if (is_asleep(solve.a) || is_asleep(solve.b)) {
            const SolveSnapshot& sa = solve_snapshot_[solve.a];
            const SolveSnapshot& sb = solve_snapshot_[solve.b];
            const PreStep& pa = pre_step_[solve.a];
            const PreStep& pb = pre_step_[solve.b];
            const float vax = is_asleep(solve.a) ? pa.vx : sa.vx;
            const float vay = is_asleep(solve.a) ? pa.vy : sa.vy;
            const float vaz = is_asleep(solve.a) ? pa.vz : sa.vz;
            const float vbx = is_asleep(solve.b) ? pb.vx : sb.vx;
            const float vby = is_asleep(solve.b) ? pb.vy : sb.vy;
            const float vbz = is_asleep(solve.b) ? pb.vz : sb.vz;
            vel_n = (vbx - vax) * solve.nx + (vby - vay) * solve.ny +
                    (vbz - vaz) * solve.nz;
          }
          if (vel_n < 0.0f) {
            // Restitution threshold (opt-in): see the ground phase. Default
            // 0 reduces to the historical always-bounce impulse.
            const float eff_e = -vel_n > restitution_threshold_
                                    ? solve.restitution
                                    : 0.0f;
            const float inv_a =
                is_asleep(solve.a) ? 0.0f : a.inverse_mass;
            const float inv_b =
                is_asleep(solve.b) ? 0.0f : b.inverse_mass;
            const float inv_eff = inv_a + inv_b;
            if (inv_eff <= 0.0f) {
              contacts_.push_back(ContactEvent{
                  solve.a, solve.b, solve.penetration});
              continue;  // both pinned: nothing to move, event still emitted
            }
            const float impulse =
                -(1.0f + eff_e) * vel_n / inv_eff;
            if (inv_a > 0.0f) {
              a.velocity[0] -= impulse * solve.nx * inv_a;
              a.velocity[1] -= impulse * solve.ny * inv_a;
              a.velocity[2] -= impulse * solve.nz * inv_a;
            }
            if (inv_b > 0.0f) {
              b.velocity[0] += impulse * solve.nx * inv_b;
              b.velocity[1] += impulse * solve.ny * inv_b;
              b.velocity[2] += impulse * solve.nz * inv_b;
            }
            // Torque side effect on spin only (linear updates above are the
            // historical ones). J on b is +impulse*n, on a is -impulse*n.
            apply_torque(solve.b, impulse * solve.nx, impulse * solve.ny,
                         impulse * solve.nz, solve.rbx, solve.rby, solve.rbz);
            apply_torque(solve.a, -impulse * solve.nx, -impulse * solve.ny,
                         -impulse * solve.nz, solve.rax, solve.ray, solve.raz);
          }
          contacts_.push_back(ContactEvent{
              solve.a, solve.b, solve.penetration});
        }
      } else {
        solve_velocities_vfp(dt);
      }
      apply_sleep_restore();
    } else {
      // Fewer than two bodies: no pairs possible, but a lone body resting
      // on the ground still forms a single-body island that may sleep.
      solve_contacts_.clear();
      build_islands();
      if (velocity_from_position_) {
        solve_velocities_vfp(dt);
      }
      apply_sleep_restore();
    }
  }

  //! XPBD velocity-from-position stage (VFP mode only): every awake dynamic
  //! velocity is the corrected-position delta over the increment, so
  //! velocities agree with the positions the solver produced (joints
  //! included). The impulse pass does not run in this mode. Shared by the
  //! pair path and the single-body path (a lone grounded body still needs
  //! derived velocities, or gravity would accumulate in vy under a clamp).
  void solve_velocities_vfp(float dt) noexcept {
        // XPBD velocity-from-position: every awake dynamic velocity is the
        // corrected-position delta over the increment, so velocities agree
        // with the positions the solver produced (joints included). The
        // impulse pass above does not run in this mode.
        const float inv_dt = dt > 0.0F ? 1.0F / dt : 0.0F;
        for (std::size_t vi = 0; vi < bodies_.size(); ++vi) {
          if (bodies_[vi].inverse_mass <= 0.0F) continue;
          if (is_asleep(static_cast<std::uint32_t>(vi))) continue;
          const PreStep& pre = pre_step_[vi];
          PhysicsBody& b = bodies_[vi];
          b.velocity[0] = (b.position[0] - pre.px) * inv_dt;
          b.velocity[1] = (b.position[1] - pre.py) * inv_dt;
          b.velocity[2] = (b.position[2] - pre.pz) * inv_dt;
        }
        // Restitution bounce on top, from the stored pre-solve approach
        // speeds (the derived normal velocity is ~0 after projection, so
        // this sets the post-bounce separation without double-counting).
        for (const ContactSolve& solve : solve_contacts_) {
          float vel_n = solve.vel_n;
          if (is_asleep(solve.a) || is_asleep(solve.b)) {
            const SolveSnapshot& sa = solve_snapshot_[solve.a];
            const SolveSnapshot& sb = solve_snapshot_[solve.b];
            const PreStep& pa = pre_step_[solve.a];
            const PreStep& pb = pre_step_[solve.b];
            const float vax = is_asleep(solve.a) ? pa.vx : sa.vx;
            const float vay = is_asleep(solve.a) ? pa.vy : sa.vy;
            const float vaz = is_asleep(solve.a) ? pa.vz : sa.vz;
            const float vbx = is_asleep(solve.b) ? pb.vx : sb.vx;
            const float vby = is_asleep(solve.b) ? pb.vy : sb.vy;
            const float vbz = is_asleep(solve.b) ? pb.vz : sb.vz;
            vel_n = (vbx - vax) * solve.nx + (vby - vay) * solve.ny +
                    (vbz - vaz) * solve.nz;
          }
          if (vel_n < 0.0f && -vel_n > restitution_threshold_) {
            PhysicsBody& a = bodies_[solve.a];
            PhysicsBody& b = bodies_[solve.b];
            const float inv_a =
                is_asleep(solve.a) ? 0.0f : a.inverse_mass;
            const float inv_b =
                is_asleep(solve.b) ? 0.0f : b.inverse_mass;
            const float inv_eff = inv_a + inv_b;
            if (inv_eff <= 0.0f) {
              contacts_.push_back(ContactEvent{
                  solve.a, solve.b, solve.penetration});
              continue;
            }
            const float bounce = -solve.restitution * vel_n / inv_eff;
            if (inv_a > 0.0f) {
              a.velocity[0] -= bounce * solve.nx * inv_a;
              a.velocity[1] -= bounce * solve.ny * inv_a;
              a.velocity[2] -= bounce * solve.nz * inv_a;
            }
            if (inv_b > 0.0f) {
              b.velocity[0] += bounce * solve.nx * inv_b;
              b.velocity[1] += bounce * solve.ny * inv_b;
              b.velocity[2] += bounce * solve.nz * inv_b;
            }
            apply_torque(solve.b, bounce * solve.nx, bounce * solve.ny,
                         bounce * solve.nz, solve.rbx, solve.rby, solve.rbz);
            apply_torque(solve.a, -bounce * solve.nx, -bounce * solve.ny,
                         -bounce * solve.nz, solve.rax, solve.ray, solve.raz);
          }
          contacts_.push_back(ContactEvent{
              solve.a, solve.b, solve.penetration});
        }
        // Ground bounce + friction on the derived velocities.
        for (std::size_t gi = 0; gi < bodies_.size(); ++gi) {
          if (grounded_[gi] == 0U) continue;
          if (bodies_[gi].inverse_mass <= 0.0F) continue;
          if (is_asleep(static_cast<std::uint32_t>(gi))) continue;
          PhysicsBody& b = bodies_[gi];
          const float approach = grounded_approach_[gi];
          const float old_vx = b.velocity[0];
          const float old_vz = b.velocity[2];
          b.velocity[0] *= 0.98F;
          b.velocity[2] *= 0.98F;
          const float old_vy = b.velocity[1];
          b.velocity[1] =
              approach > restitution_threshold_ ? approach * b.restitution : 0.0F;
          const float inv_m = b.inverse_mass;
          if (inv_m > 0.0F) {
            const float mass = 1.0F / inv_m;
            const float jx = mass * (b.velocity[0] - old_vx);
            const float jy = mass * (b.velocity[1] - old_vy);
            const float jz = mass * (b.velocity[2] - old_vz);
            float support = b.radius;
            if (b.shape == BodyShape::Capsule && b.half_height > 0.0F) {
              float ux = 0.0F, uy = 1.0F, uz = 0.0F;
              quat_rotate_vector(b.orientation, 0.0F, 1.0F, 0.0F, ux, uy, uz);
              float auy = uy >= 0.0F ? uy : -uy;
              support = auy * b.half_height + b.radius;
            }
            apply_torque(static_cast<std::uint32_t>(gi), jx, jy, jz, 0.0F,
                         -support, 0.0F);
          }
        }
  }

  //! Disable the spatial grid and solve every pair. Slower, but the reference
  //! the broadphase must agree with: the first attempt at this was wrong at
  //! N=4000 and only a like-for-like comparison at several scales caught it,
  //! so the comparison is now reachable from a test rather than from a manual
  //! rebuild with the threshold edited.
  //! Positional-correction iterations per step (Gauss-Seidel: the contact SET
  //! is frozen from the snapshot, but each pass re-reads live positions, so
  //! later passes push out residual overlap the earlier passes created).
  //! Default 1 = the historical single pass; larger values trade step cost for
  //! stacking firmness. Deterministic at any count: fixed contact order.
  void set_solver_iterations(std::uint32_t n) noexcept {
    solver_iterations_ = n > 0U ? n : 1U;
  }
  [[nodiscard]] std::uint32_t solver_iterations() const noexcept {
    return solver_iterations_;
  }

  void set_force_all_pairs(bool force) noexcept { force_all_pairs_ = force; }
  [[nodiscard]] bool force_all_pairs() const noexcept { return force_all_pairs_; }

  //! Below this body count the all-pairs loop is cheaper than building a grid.
  static constexpr std::size_t kBroadphaseMinBodies = 24;
  //! Ceiling on candidate pairs. A pathological pile-up (every body in one
  //! cell) must not make the broadphase the O(n^2) scan it exists to avoid, so
  //! past this the full pair list is built instead -- slower, but correct.
  static constexpr std::size_t kBroadphaseMaxCandidates = 1U << 18U;

  //! One frozen body state for contact detection. Detection must not read live
  //! positions: see the step-3 comment.
  struct SolveSnapshot {
    float px{0.0f}, py{0.0f}, pz{0.0f};
    float vx{0.0f}, vy{0.0f}, vz{0.0f};
    float radius{0.0f};
    float inverse_mass{0.0f};
    float restitution{0.0f};
    BodyShape shape{BodyShape::Sphere};
    float half_height{0.0f};
    float qx{0.0f}, qy{0.0f}, qz{0.0f}, qw{1.0f};
    float bound{0.0f};  //!< bounding radius (broadphase + precheck)
  };
  //! One resolved contact, computed from the snapshot and then applied.
  //! r_a/r_b are the contact-point lever arms (world frame) for the torque
  //! side effect on spin; the linear solve is unchanged by them.
  //! A speculative contact (R3/D1) stopped mid-flight: `speculative` marks
  //! a TOI rewind instead of a separation. `penetration` is 0 (touching at
  //! TOI, so no island edge — conservative: a speculatively touched island
  //! stays awake), and (wx, wy, wz) is the relative overshoot vector from
  //! the TOI configuration to the snapshot end configuration. Pass 0
  //! rewinds it split by inverse mass (center-of-mass preserving; exact
  //! for a static obstacle or head-on equal masses); the impulse then
  //! kills the approach velocity exactly like a discrete contact.
  struct ContactSolve {
    std::uint32_t a{0};
    std::uint32_t b{0};
    float nx{0.0f}, ny{0.0f}, nz{0.0f};
    float penetration{0.0f};
    float inv_sum{0.0f};
    float restitution{0.0f};
    float vel_n{0.0f};
    float rax{0.0f}, ray{0.0f}, raz{0.0f};
    float rbx{0.0f}, rby{0.0f}, rbz{0.0f};
    bool speculative{false};
    float wx{0.0f}, wy{0.0f}, wz{0.0f};
  };
  //! Exact pre-integration pose+velocity of one body (the sleeper restore
  //! source and the rest-test input). Radii/masses/shapes are immutable
  //! mid-run, so only the integrated quantities are stored.
  struct PreStep {
    float px{0.0f}, py{0.0f}, pz{0.0f};
    float vx{0.0f}, vy{0.0f}, vz{0.0f};
    float qx{0.0f}, qy{0.0f}, qz{0.0f}, qw{1.0f};
    float wx{0.0f}, wy{0.0f}, wz{0.0f};
  };

  //! Capsule axis (unit local Y in world frame) from a snapshot orientation.
  void snapshot_axis(const SolveSnapshot& s, float& ux, float& uy,
                     float& uz) const noexcept {
    float q[4] = {s.qx, s.qy, s.qz, s.qw};
    quat_rotate_vector(q, 0.0F, 1.0F, 0.0F, ux, uy, uz);
  }

  //! Closest points between segments p1-q1 and p2-q2 (Ericson,
  //! Real-Time Collision Detection §5.1.8 — standard, cited not novel).
  //! Deterministic: fixed branch structure, no iteration, float32 throughout.
  static void segment_segment_closest(float p1x, float p1y, float p1z,
                                      float q1x, float q1y, float q1z,
                                      float p2x, float p2y, float p2z,
                                      float q2x, float q2y, float q2z,
                                      float& c1x, float& c1y, float& c1z,
                                      float& c2x, float& c2y, float& c2z) noexcept {
    const float d1x = q1x - p1x, d1y = q1y - p1y, d1z = q1z - p1z;
    const float d2x = q2x - p2x, d2y = q2y - p2y, d2z = q2z - p2z;
    const float rx = p1x - p2x, ry = p1y - p2y, rz = p1z - p2z;
    const float a = d1x * d1x + d1y * d1y + d1z * d1z;
    const float e = d2x * d2x + d2y * d2y + d2z * d2z;
    const float f = d2x * rx + d2y * ry + d2z * rz;
    float s = 0.0F, t = 0.0F;
    if (a <= 1e-8F && e <= 1e-8F) {
      s = 0.0F;
      t = 0.0F;
    } else if (a <= 1e-8F) {
      s = 0.0F;
      t = f / e;
      t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
    } else {
      const float c = d1x * rx + d1y * ry + d1z * rz;
      if (e <= 1e-8F) {
        t = 0.0F;
        s = -c / a;
        s = s < 0.0F ? 0.0F : (s > 1.0F ? 1.0F : s);
      } else {
        const float b = d1x * d2x + d1y * d2y + d1z * d2z;
        const float denom = a * e - b * b;
        s = denom > 1e-8F ? (b * f - c * e) / denom : 0.0F;
        s = s < 0.0F ? 0.0F : (s > 1.0F ? 1.0F : s);
        t = (b * s + f) / e;
        if (t < 0.0F) {
          t = 0.0F;
          s = -c / a;
          s = s < 0.0F ? 0.0F : (s > 1.0F ? 1.0F : s);
        } else if (t > 1.0F) {
          t = 1.0F;
          s = (b - c) / a;
          s = s < 0.0F ? 0.0F : (s > 1.0F ? 1.0F : s);
        }
      }
    }
    c1x = p1x + d1x * s;
    c1y = p1y + d1y * s;
    c1z = p1z + d1z * s;
    c2x = p2x + d2x * t;
    c2y = p2y + d2y * t;
    c2z = p2z + d2z * t;
  }

  //! Swept entry root for a relative segment against radius R: the first
  //! s in [0, 1] with |c0 + d*s| = R, where d = (dx, dy, dz) is the given
  //! relative travel. Pure algebra (quadratic formula), deterministic
  //! float32 throughout, one shared implementation so the sphere and
  //! capsule branches cannot drift (the BLAS-size duplication lesson).
  //! Start-inside (c0 <= R) is NOT an entry: returns false, leaving the
  //! pair to the discrete path (continuing overlap) or the historical miss
  //! (separating from overlap — solving the exit root would glue a
  //! legitimately separating pair back). s_out is written only on success.
  static bool swept_entry_root(float c0x, float c0y, float c0z, float dx,
                               float dy, float dz, float r_sq,
                               float& s_out) noexcept {
    const float c0sq = c0x * c0x + c0y * c0y + c0z * c0z;
    if (c0sq <= r_sq) return false;
    const float ddx = dx - c0x;
    const float ddy = dy - c0y;
    const float ddz = dz - c0z;
    const float a2 = ddx * ddx + ddy * ddy + ddz * ddz;
    if (a2 <= 0.0f) return false;
    const float bq = c0x * ddx + c0y * ddy + c0z * ddz;
    if (bq >= 0.0f) return false;
    const float disc = bq * bq - a2 * (c0sq - r_sq);
    if (disc < 0.0f) return false;
    const float s = (-bq - std::sqrt(disc)) / a2;
    if (s < 0.0f || s > 1.0f) return false;
    s_out = s;
    return true;
  }

  //! Shape-aware narrowphase on the frozen snapshot. Sphere-sphere is the
  //! historical computation exactly (same degenerate branch, same order);
  //! any pair involving a capsule reduces to sphere-vs-sphere at the closest
  //! segment points. Returns false when separated or both static. A
  //! bounding-sphere precheck (centers vs bound sum) runs first — it can
  //! only reject pairs the exact test would reject.
  //! Substep length h locates each body's segment start (p - v*h) for the
  //! swept test; the discrete path ignores it.
  bool narrowphase_pair(std::size_t i, std::size_t j, ContactSolve& solve,
                        float h) noexcept {
    const SolveSnapshot& a = solve_snapshot_[i];
    const SolveSnapshot& b = solve_snapshot_[j];
    const float inv_sum = a.inverse_mass + b.inverse_mass;
    if (inv_sum <= 0.0f) return false;  // both static
    const bool cap_a = a.shape == BodyShape::Capsule && a.half_height > 0.0F;
    const bool cap_b = b.shape == BodyShape::Capsule && b.half_height > 0.0F;
    if (!cap_a && !cap_b) {
      const float dx = b.px - a.px;
      const float dy = b.py - a.py;
      const float dz = b.pz - a.pz;
      const float dist_sq = dx * dx + dy * dy + dz * dz;
      const float min_dist = a.radius + b.radius;
      if (min_dist <= 0.0f) return false;
      // Relative segment c0 -> c1 over the substep, from snapshot end
      // positions p and velocities v: c0 = (pb - vb*h) - (pa - va*h),
      // c1 = (dx, dy, dz) above.
      const float c0x = (b.px - b.vx * h) - (a.px - a.vx * h);
      const float c0y = (b.py - b.vy * h) - (a.py - a.vy * h);
      const float c0z = (b.pz - b.vz * h) - (a.pz - a.vz * h);
      const float c0sq = c0x * c0x + c0y * c0y + c0z * c0z;
      const float r_sq = min_dist * min_dist;
      // The discrete contact, computed once and returned for every
      // end-inside pair the swept rule does not own. Verbatim historical
      // arithmetic, so flag-off behavior (and start-inside overlap with
      // flag on) is bit-identical to before.
      const auto discrete = [&]() {
        solve.a = static_cast<std::uint32_t>(i);
        solve.b = static_cast<std::uint32_t>(j);
        const float dist = std::sqrt(dist_sq);
        // Degenerate: coincident centers -> separate along +Y.
        solve.nx = dist > 1e-8f ? dx / dist : 0.0f;
        solve.ny = dist > 1e-8f ? dy / dist : 1.0f;
        solve.nz = dist > 1e-8f ? dz / dist : 0.0f;
        solve.penetration = min_dist - dist;
        solve.inv_sum = inv_sum;
        solve.restitution = 0.5f * (a.restitution + b.restitution);
        solve.vel_n = (b.vx - a.vx) * solve.nx + (b.vy - a.vy) * solve.ny +
                      (b.vz - a.vz) * solve.nz;
        // Lever arms: contact point on a's surface along +n.
        solve.rax = solve.nx * a.radius;
        solve.ray = solve.ny * a.radius;
        solve.raz = solve.nz * a.radius;
        solve.rbx = solve.nx * -b.radius;
        solve.rby = solve.ny * -b.radius;
        solve.rbz = solve.nz * -b.radius;
      };
      // Swept (speculative) TOI contact. Owns pairs that ENTERED mid-step
      // (start outside, end inside or through): the discrete end-normal
      // points the wrong way for far-half samples (separating along it
      // exits the far side — the ejection mechanism), so TOI rewind +
      // approach-kill is the correct response. Start-inside pairs stay
      // discrete (resting/continuing overlap); start-overlap/end-
      // separated pairs keep the historical miss (solving the exit root
      // would glue a legitimately separating pair back).
      const bool crossed = dist_sq < r_sq && c0sq > r_sq;
      if (speculative_contacts_ && (crossed || dist_sq >= r_sq) &&
          c0sq > r_sq) {
        float s = -1.0f;
        const bool entered =
            swept_entry_root(c0x, c0y, c0z, dx, dy, dz, r_sq, s);
        if (entered) {
          const float ddx = dx - c0x;
          const float ddy = dy - c0y;
          const float ddz = dz - c0z;
          // TOI configuration: centers exactly min_dist apart along n.
          const float cx = c0x + ddx * s;
          const float cy = c0y + ddy * s;
          const float cz = c0z + ddz * s;
          solve.a = static_cast<std::uint32_t>(i);
          solve.b = static_cast<std::uint32_t>(j);
          solve.nx = cx / min_dist;
          solve.ny = cy / min_dist;
          solve.nz = cz / min_dist;
          solve.penetration = 0.0f;  // touching at TOI; never an island edge
          solve.inv_sum = inv_sum;
          solve.restitution = 0.5f * (a.restitution + b.restitution);
          solve.vel_n = (b.vx - a.vx) * solve.nx + (b.vy - a.vy) * solve.ny +
                        (b.vz - a.vz) * solve.nz;
          if (solve.vel_n < 0.0f) {
            solve.rax = solve.nx * a.radius;
            solve.ray = solve.ny * a.radius;
            solve.raz = solve.nz * a.radius;
            solve.rbx = solve.nx * -b.radius;
            solve.rby = solve.ny * -b.radius;
            solve.rbz = solve.nz * -b.radius;
            // Relative overshoot from TOI to segment end; pass 0 rewinds
            // it split by inverse mass (COM-preserving; exact against a
            // static obstacle or head-on equal masses).
            const float k = 1.0f - s;
            solve.speculative = true;
            solve.wx = ddx * k;
            solve.wy = ddy * k;
            solve.wz = ddz * k;
            return true;
          }
          // At TOI but separating (float dust on a grazing entry): fall
          // through to discrete/miss below.
        }
      }
      if (dist_sq < r_sq) {
        discrete();
        return true;
      }
      return false;
    }
    // Capsule-involved: endpoints in world frame, then closest points.
    float aux = 0.0F, auy = 1.0F, auz = 0.0F;
    float bux = 0.0F, buy = 1.0F, buz = 0.0F;
    if (cap_a) snapshot_axis(a, aux, auy, auz);
    if (cap_b) snapshot_axis(b, bux, buy, buz);
    const float a0x = a.px - aux * a.half_height;
    const float a0y = a.py - auy * a.half_height;
    const float a0z = a.pz - auz * a.half_height;
    const float a1x = a.px + aux * a.half_height;
    const float a1y = a.py + auy * a.half_height;
    const float a1z = a.pz + auz * a.half_height;
    const float b0x = b.px - bux * b.half_height;
    const float b0y = b.py - buy * b.half_height;
    const float b0z = b.pz - buz * b.half_height;
    const float b1x = b.px + bux * b.half_height;
    const float b1y = b.py + buy * b.half_height;
    const float b1z = b.pz + buz * b.half_height;
    float cax = a.px, cay = a.py, caz = a.pz;
    float cbx = b.px, cby = b.py, cbz = b.pz;
    if (cap_a || cap_b) {
      segment_segment_closest(a0x, a0y, a0z, a1x, a1y, a1z, b0x, b0y, b0z,
                              b1x, b1y, b1z, cax, cay, caz, cbx, cby, cbz);
    }
    const float dx = cbx - cax;
    const float dy = cby - cay;
    const float dz = cbz - caz;
    const float dist_sq = dx * dx + dy * dy + dz * dz;
    const float min_dist = a.radius + b.radius;
    if (min_dist <= 0.0f) return false;
    const float cap_r_sq = min_dist * min_dist;
    if (dist_sq < cap_r_sq) {
      // End overlap: the discrete segment contact, verbatim — unless the
      // pair crossed this step (start outside the bound spheres), in
      // which case the discrete end-normal exits the far side and the TOI
      // path below owns it (same ejection mechanism as spheres).
      const float sc0x = (b.px - b.vx * h) - (a.px - a.vx * h);
      const float sc0y = (b.py - b.vy * h) - (a.py - a.vy * h);
      const float sc0z = (b.pz - b.vz * h) - (a.pz - a.vz * h);
      const float sc0sq = sc0x * sc0x + sc0y * sc0y + sc0z * sc0z;
      const float bound_sum = a.bound + b.bound;
      if (!speculative_contacts_ || sc0sq <= bound_sum * bound_sum) {
        solve.a = static_cast<std::uint32_t>(i);
        solve.b = static_cast<std::uint32_t>(j);
        const float dist = std::sqrt(dist_sq);
        solve.nx = dist > 1e-8f ? dx / dist : 0.0f;
        solve.ny = dist > 1e-8f ? dy / dist : 1.0f;
        solve.nz = dist > 1e-8f ? dz / dist : 0.0f;
        solve.penetration = min_dist - dist;
        solve.inv_sum = inv_sum;
        solve.restitution = 0.5f * (a.restitution + b.restitution);
        solve.vel_n = (b.vx - a.vx) * solve.nx + (b.vy - a.vy) * solve.ny +
                      (b.vz - a.vz) * solve.nz;
        const float cpx = cax + solve.nx * a.radius;
        const float cpy = cay + solve.ny * a.radius;
        const float cpz = caz + solve.nz * a.radius;
        solve.rax = cpx - a.px;
        solve.ray = cpy - a.py;
        solve.raz = cpz - a.pz;
        solve.rbx = cpx - b.px;
        solve.rby = cpy - b.py;
        solve.rbz = cpz - b.pz;
        return true;
      }
      // Crossed: fall through to the TOI path (bound trigger below finds
      // the entry; the start-outside check is already established here).
    } else if (!speculative_contacts_) {
      return false;
    }
    // Capsule swept TOI (flag on). Bound-sphere entry on centers first
    // (conservative trigger: bound spheres contain the capsules, and the
    // grid already guarantees the pair was gathered), then Newton
    // refinement on the true segment gap at fixed END orientations —
    // rotation within a substep is ignored (exact for non-spinning pairs;
    // documented approximation otherwise). Fixed 3 iterations:
    // deterministic best-effort; a non-converged graze misses this step
    // and is re-tested next step (delays rather than tunnels).
    const float tc0x = (b.px - b.vx * h) - (a.px - a.vx * h);
    const float tc0y = (b.py - b.vy * h) - (a.py - a.vy * h);
    const float tc0z = (b.pz - b.vz * h) - (a.pz - a.vz * h);
    const float tc1x = b.px - a.px;
    const float tc1y = b.py - a.py;
    const float tc1z = b.pz - a.pz;
    // Segment axes at END orientations (fixed through the trigger and the
    // refinement below).
    float tax = 0.0F, tay = 1.0F, taz = 0.0F;
    float tbx = 0.0F, tby = 1.0F, tbz = 0.0F;
    if (cap_a) snapshot_axis(a, tax, tay, taz);
    if (cap_b) snapshot_axis(b, tbx, tby, tbz);
    // Start-inside-bound is transit, not rest: the bound was entered on
    // an earlier step without surface contact (phase 7 of the gate does
    // exactly this). Only start-inside-SURFACE keeps the historical miss
    // (resting/separating — and the same corner the sphere rule takes).
    // The surface gap at step start costs one extra closest-points solve,
    // but only for bound-overlapping pairs.
    const float bound_sum = a.bound + b.bound;
    const float bound_r_sq = bound_sum * bound_sum;
    const float tc0sq = tc0x * tc0x + tc0y * tc0y + tc0z * tc0z;
    float sb = -1.0f;
    if (tc0sq > bound_r_sq) {
      if (!swept_entry_root(tc0x, tc0y, tc0z, tc1x, tc1y, tc1z, bound_r_sq,
                            sb)) {
        return false;
      }
    } else {
      const float sax = a.px - a.vx * h;
      const float say = a.py - a.vy * h;
      const float saz = a.pz - a.vz * h;
      const float sbx = b.px - b.vx * h;
      const float sby = b.py - b.vy * h;
      const float sbz = b.pz - b.vz * h;
      float scax = sax, scay = say, scaz = saz;
      float scbx = sbx, scby = sby, scbz = sbz;
      segment_segment_closest(
          sax - tax * a.half_height, say - tay * a.half_height,
          saz - taz * a.half_height, sax + tax * a.half_height,
          say + tay * a.half_height, saz + taz * a.half_height,
          sbx - tbx * b.half_height, sby - tby * b.half_height,
          sbz - tbz * b.half_height, sbx + tbx * b.half_height,
          sby + tby * b.half_height, sbz + tbz * b.half_height, scax, scay,
          scaz, scbx, scby, scbz);
      const float sgx = scbx - scax;
      const float sgy = scby - scay;
      const float sgz = scbz - scaz;
      if (sgx * sgx + sgy * sgy + sgz * sgz <= min_dist * min_dist) {
        return false;  // resting/separating at step start: historical miss
      }
      sb = 0.0f;  // surface-separated inside the bound: Newton climbs
    }
    float s = sb;
    for (int newton = 0; newton < 3; ++newton) {
      const float k = 1.0f - s;
      const float pax = a.px - a.vx * h * k;
      const float pay = a.py - a.vy * h * k;
      const float paz = a.pz - a.vz * h * k;
      const float pbx = b.px - b.vx * h * k;
      const float pby = b.py - b.vy * h * k;
      const float pbz = b.pz - b.vz * h * k;
      float qax = pax, qay = pay, qaz = paz;
      float qbx = pbx, qby = pby, qbz = pbz;
      segment_segment_closest(pax - tax * a.half_height,
                              pay - tay * a.half_height,
                              paz - taz * a.half_height,
                              pax + tax * a.half_height,
                              pay + tay * a.half_height,
                              paz + taz * a.half_height,
                              pbx - tbx * b.half_height,
                              pby - tby * b.half_height,
                              pbz - tbz * b.half_height,
                              pbx + tbx * b.half_height,
                              pby + tby * b.half_height,
                              pbz + tbz * b.half_height,
                              qax, qay, qaz, qbx, qby, qbz);
      const float gx = qbx - qax;
      const float gy = qby - qay;
      const float gz = qbz - qaz;
      const float gdist_sq = gx * gx + gy * gy + gz * gz;
      const float gdist = std::sqrt(gdist_sq);
      if (gdist <= min_dist) break;  // at/past the surface: solve here
      // Advance along the approach: gap over center-closing-rate. The
      // rate uses centers (first-order; closest-point migration within
      // one Newton step is second-order). Sign: the gap SHRINKS as s
      // grows while closing, so the closing rate is minus the relative
      // velocity along the outward closest-point normal.
      float nnx = gx, nny = gy, nnz = gz;
      if (gdist > 1e-8f) {
        nnx /= gdist;
        nny /= gdist;
        nnz /= gdist;
      }
      const float rate = -((b.vx - a.vx) * nnx + (b.vy - a.vy) * nny +
                           (b.vz - a.vz) * nnz) *
                         h;
      if (rate <= 1e-9f) break;  // not closing: take the current s
      s += (gdist - min_dist) / rate;
      if (s < 0.0f) {
        s = 0.0f;
      } else if (s > 1.0f) {
        s = 1.0f;
      }
    }
    // Capsule geometry at the refined TOI centers + end orientations.
    {
      const float k = 1.0f - s;
      const float pax = a.px - a.vx * h * k;
      const float pay = a.py - a.vy * h * k;
      const float paz = a.pz - a.vz * h * k;
      const float pbx = b.px - b.vx * h * k;
      const float pby = b.py - b.vy * h * k;
      const float pbz = b.pz - b.vz * h * k;
      float qax = pax, qay = pay, qaz = paz;
      float qbx = pbx, qby = pby, qbz = pbz;
      segment_segment_closest(pax - tax * a.half_height,
                              pay - tay * a.half_height,
                              paz - taz * a.half_height,
                              pax + tax * a.half_height,
                              pay + tay * a.half_height,
                              paz + taz * a.half_height,
                              pbx - tbx * b.half_height,
                              pby - tby * b.half_height,
                              pbz - tbz * b.half_height,
                              pbx + tbx * b.half_height,
                              pby + tby * b.half_height,
                              pbz + tbz * b.half_height,
                              qax, qay, qaz, qbx, qby, qbz);
      const float fx = qbx - qax;
      const float fy = qby - qay;
      const float fz = qbz - qaz;
      const float fdist_sq = fx * fx + fy * fy + fz * fz;
      const float fdist = std::sqrt(fdist_sq);
      // Non-converged graze (still clearly separated): miss this step.
      if (fdist > min_dist + 1e-3f) return false;
      solve.a = static_cast<std::uint32_t>(i);
      solve.b = static_cast<std::uint32_t>(j);
      solve.nx = fdist > 1e-8f ? fx / fdist : 0.0f;
      solve.ny = fdist > 1e-8f ? fy / fdist : 1.0f;
      solve.nz = fdist > 1e-8f ? fz / fdist : 0.0f;
      solve.penetration = 0.0f;  // touching at TOI; never an island edge
      solve.inv_sum = inv_sum;
      solve.restitution = 0.5f * (a.restitution + b.restitution);
      solve.vel_n = (b.vx - a.vx) * solve.nx + (b.vy - a.vy) * solve.ny +
                    (b.vz - a.vz) * solve.nz;
      if (solve.vel_n >= 0.0f) return false;
      // Lever arms from the TOI contact point (torque side effect only).
      const float cpx = qax + solve.nx * a.radius;
      const float cpy = qay + solve.ny * a.radius;
      const float cpz = qaz + solve.nz * a.radius;
      solve.rax = cpx - pax;
      solve.ray = cpy - pay;
      solve.raz = cpz - paz;
      solve.rbx = cpx - pbx;
      solve.rby = cpy - pby;
      solve.rbz = cpz - pbz;
      // Center rewind from TOI to end, split by inverse mass (same
      // COM-preserving rewind as spheres; orientations untouched).
      solve.speculative = true;
      solve.wx = (tc1x - tc0x) * k;
      solve.wy = (tc1y - tc0y) * k;
      solve.wz = (tc1z - tc0z) * k;
      return true;
    }
  }

  //! Fill candidate_pairs_ with (i, j), i < j, in exactly the order the nested
  //! all-pairs loop would visit them, containing every pair that can overlap.
  //!
  //! This is only sound because step 3 detects against a frozen snapshot: the
  //! grid is built from those same snapshot positions, so "pairs within
  //! distance 2*max_radius of a cell" provably contains every overlapping pair
  //! and nothing depends on positions moving during the pass. An earlier
  //! version that resolved live was NOT equivalent -- it matched at N=1000 and
  //! diverged at N=4000.
  //!
  //! Determinism is why this is a sorted vector rather than a hash grid walked
  //! in bucket order: hash iteration order is unspecified, and the contact set
  //! is order-sensitive once corrections are applied.
  void rebuild_candidate_pairs(float h) noexcept {
    candidate_pairs_.clear();
    const std::size_t count = solve_snapshot_.size();
    if (count < 2U) return;

    // Cell size at least two maximum bounding radii, so any two overlapping
    // bounding spheres are in the same or an adjacent cell (capsules use
    // radius + half-height; spheres are unchanged).
    float max_radius = 0.0f;
    for (const SolveSnapshot& body : solve_snapshot_) {
      if (body.bound > max_radius) max_radius = body.bound;
    }
    if (max_radius <= 0.0f) return;  // nothing can overlap

    if (force_all_pairs_ || count < kBroadphaseMinBodies) {
      candidate_pairs_.reserve(count * (count - 1U) / 2U);
      for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
          candidate_pairs_.emplace_back(static_cast<std::uint32_t>(i),
                                       static_cast<std::uint32_t>(j));
        }
      }
      return;
    }

    const float cell = max_radius * 2.0f;
    // Floor, not truncation: negative coordinates must not fold toward zero,
    // or bodies at -0.5 and +0.5 could land in non-adjacent cells and the pair
    // would be missed.
    const auto cell_of = [cell](float v) {
      return static_cast<std::int64_t>(std::floor(v / cell));
    };

    grid_.reserve_for(count);
    grid_.clear_frame();
    // Swept footprints (speculative mode): each body occupies every cell
    // its bound-sphere sweep touches, so a pair whose paths cross is
    // guaranteed a shared cell. A swept pair overlaps at some point X; X
    // is within bound_a of a's segment and within bound_b of b's segment,
    // so both bodies' swept ranges contain cell(X). The bound sphere
    // conservatively covers capsules (bound = radius + half-height).
    // Discrete mode keeps the historical single-cell insert verbatim.
    std::vector<CellRange> ranges{};
    if (speculative_contacts_) {
      ranges.resize(count);
      for (std::size_t i = 0; i < count; ++i) {
        const SolveSnapshot& body = solve_snapshot_[i];
        const float sx = body.px - body.vx * h;
        const float sy = body.py - body.vy * h;
        const float sz = body.pz - body.vz * h;
        CellRange& r = ranges[i];
        r.lox = cell_of((sx < body.px ? sx : body.px) - body.bound);
        r.hix = cell_of((sx > body.px ? sx : body.px) + body.bound);
        r.loy = cell_of((sy < body.py ? sy : body.py) - body.bound);
        r.hiy = cell_of((sy > body.py ? sy : body.py) + body.bound);
        r.loz = cell_of((sz < body.pz ? sz : body.pz) - body.bound);
        r.hiz = cell_of((sz > body.pz ? sz : body.pz) + body.bound);
        for (std::int64_t cx = r.lox; cx <= r.hix; ++cx) {
          for (std::int64_t cy = r.loy; cy <= r.hiy; ++cy) {
            for (std::int64_t cz = r.loz; cz <= r.hiz; ++cz) {
              grid_.insert({cx, cy, cz})
                  .members.push_back(static_cast<std::uint32_t>(i));
            }
          }
        }
      }
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        const SolveSnapshot& body = solve_snapshot_[i];
        grid_
            .insert({cell_of(body.px), cell_of(body.py), cell_of(body.pz)})
            .members.push_back(static_cast<std::uint32_t>(i));
      }
    }

    static constexpr std::int64_t kOffsets[27][3] = {
        {-1, -1, -1}, {-1, -1, 0}, {-1, -1, 1}, {-1, 0, -1}, {-1, 0, 0},
        {-1, 0, 1},   {-1, 1, -1}, {-1, 1, 0}, {-1, 1, 1}, {0, -1, -1},
        {0, -1, 0},   {0, -1, 1},   {0, 0, -1},  {0, 0, 0},  {0, 0, 1},
        {0, 1, -1},   {0, 1, 0},    {0, 1, 1},   {1, -1, -1}, {1, -1, 0},
        {1, -1, 1},   {1, 0, -1},   {1, 0, 0},   {1, 0, 1},   {1, 1, -1},
        {1, 1, 0},    {1, 1, 1}};

    // Swept query mirrors the swept insert: body i scans its own swept
    // range, so any pair sharing a swept cell is found by min(i, j)'s scan
    // (the other endpoint's insert covered that cell by the argument
    // above). Discrete mode keeps the historical 27-neighbourhood walk.
    // A swept pair can share SEVERAL cells and would be emitted once per
    // shared cell; the trailing sort+unique collapses those (the discrete
    // path emits each pair once, so unique is a no-op there).
    const auto emit_cell =
        [&](const BroadphaseGrid::Slot* slot, std::size_t i,
            bool& overflowed) {
          if (slot == nullptr) return;
          for (const std::uint32_t other : slot->members) {
            if (other <= i)
              continue;  // forward pairs only, so each appears once
            candidate_pairs_.emplace_back(static_cast<std::uint32_t>(i),
                                          other);
            if (candidate_pairs_.size() >= kBroadphaseMaxCandidates) {
              overflowed = true;
              return;
            }
          }
        };
    bool overflowed = false;
    for (std::size_t i = 0; i < count && !overflowed; ++i) {
      if (speculative_contacts_) {
        const CellRange& r = ranges[i];
        for (std::int64_t cx = r.lox; cx <= r.hix && !overflowed; ++cx) {
          for (std::int64_t cy = r.loy; cy <= r.hiy && !overflowed; ++cy) {
            for (std::int64_t cz = r.loz; cz <= r.hiz && !overflowed; ++cz) {
              emit_cell(grid_.find({cx, cy, cz}), i, overflowed);
            }
          }
        }
        continue;
      }
      const SolveSnapshot& body = solve_snapshot_[i];
      const std::int64_t cx = cell_of(body.px);
      const std::int64_t cy = cell_of(body.py);
      const std::int64_t cz = cell_of(body.pz);
      for (const auto& offset : kOffsets) {
        const BroadphaseGrid::Slot* slot =
            grid_.find({cx + offset[0], cy + offset[1], cz + offset[2]});
        if (slot == nullptr) continue;
        for (const std::uint32_t other : slot->members) {
          if (other <= i) continue;  // forward pairs only, so each appears once
          candidate_pairs_.emplace_back(static_cast<std::uint32_t>(i), other);
          if (candidate_pairs_.size() >= kBroadphaseMaxCandidates) {
            overflowed = true;
            break;
          }
        }
        if (overflowed) break;
      }
    }

    if (overflowed) {
      // Degenerate density: fall back to the complete list rather than
      // silently dropping pairs, which is the failure mode that made the
      // first attempt wrong at high N.
      candidate_pairs_.clear();
      candidate_pairs_.reserve(count * (count - 1U) / 2U);
      for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
          candidate_pairs_.emplace_back(static_cast<std::uint32_t>(i),
                                       static_cast<std::uint32_t>(j));
        }
      }
      return;
    }

    std::sort(candidate_pairs_.begin(), candidate_pairs_.end());
    // A swept pair can share several cells and is emitted once per shared
    // cell; collapse those. The discrete path emits each pair exactly once
    // (one insert cell, one query hit), so this is a no-op there — the
    // trailing output is byte-equal with or without it.
    candidate_pairs_.erase(
        std::unique(candidate_pairs_.begin(), candidate_pairs_.end()),
        candidate_pairs_.end());
  }

  //! 64-bit FNV-1a over all body positions: the determinism fingerprint
  //! (identical runs must hash identically; also used as a telemetry field).
  //! Unchanged by rotation state: orientation/omega ride in the extended
  //! fingerprint below, so historical position pins still hold.
  [[nodiscard]] std::uint64_t position_fingerprint() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const PhysicsBody& b : bodies_) {
      for (const float c : b.position) {
        std::uint32_t bits;
        static_assert(sizeof(bits) == sizeof(float));
        __builtin_memcpy(&bits, &c, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
          hash ^= static_cast<std::uint64_t>((bits >> (8 * k)) & 0xFFu);
          hash *= 1099511628211ULL;
        }
      }
    }
    return hash;
  }

  //! Extended fingerprint: positions plus orientation and spin. The
  //! determinism-check validator compares this (a velocity-only divergence
  //! with identical positions still surfaces, one step later at most, via
  //! position integration — and spin diverges positions only through
  //! capsule geometry, so spin is hashed directly).
  [[nodiscard]] std::uint64_t state_fingerprint() const noexcept {
    std::uint64_t hash = position_fingerprint();
    for (const PhysicsBody& b : bodies_) {
      for (const float c : b.orientation) {
        std::uint32_t bits;
        static_assert(sizeof(bits) == sizeof(float));
        __builtin_memcpy(&bits, &c, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
          hash ^= static_cast<std::uint64_t>((bits >> (8 * k)) & 0xFFu);
          hash *= 1099511628211ULL;
        }
      }
      for (const float c : b.angular_velocity) {
        std::uint32_t bits;
        static_assert(sizeof(bits) == sizeof(float));
        __builtin_memcpy(&bits, &c, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
          hash ^= static_cast<std::uint64_t>((bits >> (8 * k)) & 0xFFu);
          hash *= 1099511628211ULL;
        }
      }
    }
    return hash;
  }

  static constexpr std::uint32_t kWorldContact = 0xFFFFFFFFu;

 private:
  //! The substep loop behind step(). Split out so the determinism-check
  //! path can run the identical sequence twice from a saved state.
  void run_substeps(float dt) noexcept {
    const std::uint32_t n = substeps_;
    const float sub_dt = dt / static_cast<float>(n);
    for (std::uint32_t sub = 0; sub < n; ++sub) {
      step_single(sub_dt);
    }
  }

  //! Island rebuild (D1, Jolt's per-step pattern): union-find over the
  //! frozen contact graph — every solve pair plus one edge per grounded
  //! body to a synthetic world node. An island sleeps when every dynamic
  //! member's PRE-integration speed (pre_step_) is below sleep_threshold_
  //! and the island owns at least one contact edge. Union by min-index
  //! with path halving: the result depends only on the edge SET, never on
  //! insertion order, and both inputs (solve_contacts_ order, grounded_
  //! index order) are fixed — so the sleep set is a pure deterministic
  //! function of the step's inputs. Gated on sleep_threshold_ > 0: with
  //! the default 0 the sleep set stays empty and the step is untouched.
  //!
  //! Edges carry a penetration gate (kSleepEdgeMinPenetration): float32
  //! dust creates phantom ~1e-8 contacts between bodies spawned exactly
  //! touching, and a motionless spawn has pre-V speed 0 — without the gate
  //! the two combine to pin bodies MID-AIR on step 1 (measured: a column
  //! asleep at its initial 0.51/1.51 poses, floating 1cm up, with the layers
  //! above bouncing on it in a 2-step limit cycle forever). A genuine
  //! resting contact penetrates ~1e-4 per step; the gate sits two orders
  //! of magnitude on either side of the two regimes.
  //! Known limit: a body spawned motionless AND deeply overlapping sleeps
  //! in place instead of resolving first — spawns should be clean (as in
  //! the tests). Fixing that needs a sleep timer, which is stored state;
  //! see the roadmap note.
  void build_islands() noexcept {
    sleeping_flags_.assign(bodies_.size(), 0U);
    sleeping_count_ = 0;
    if (sleep_threshold_ <= 0.0f) return;
    const std::uint32_t n = static_cast<std::uint32_t>(bodies_.size());
    // Extra node N = the static world (ground plane). Grounded bodies
    // union with it, so a pile on the floor is one island.
    island_parent_.resize(static_cast<std::size_t>(n) + 1U);
    island_has_edge_.assign(static_cast<std::size_t>(n) + 1U, 0U);
    for (std::uint32_t i = 0; i <= n; ++i) island_parent_[i] = i;
    const auto find_root = [this](std::uint32_t x) {
      std::uint32_t r = x;
      while (island_parent_[r] != r) r = island_parent_[r];
      while (island_parent_[x] != r) {  // path halving
        const std::uint32_t next = island_parent_[x];
        island_parent_[x] = r;
        x = next;
      }
      return r;
    };
    const auto unite = [&](std::uint32_t a, std::uint32_t b) {
      const std::uint32_t ra = find_root(a);
      const std::uint32_t rb = find_root(b);
      // Union by min-index: order-independent, deterministic.
      if (ra < rb) {
        island_parent_[rb] = ra;
      } else if (rb < ra) {
        island_parent_[ra] = rb;
      }
      island_has_edge_[find_root(a)] = 1U;
    };
    for (const ContactSolve& solve : solve_contacts_) {
      // Penetration gate: dust contacts never form island edges (see above).
      // The SOLVE still resolves them; only sleep membership is gated.
      if (solve.penetration > kSleepEdgeMinPenetration) {
        unite(solve.a, solve.b);
      }
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      if (grounded_depth_[i] > kSleepEdgeMinPenetration) unite(i, n);
    }
    // Joints are island edges too (no penetration gate: a joint is a
    // persistent connection, not a transient touch). A pendulum hanging
    // from a static anchor is one island with the world node only when it
    // also touches; otherwise the pair still sleeps/wakes as a unit.
    for (const DistanceJoint& joint : joints_) {
      if (joint.a >= n || joint.b >= n || joint.a == joint.b) continue;
      unite(joint.a, joint.b);
    }
    const float thr_sq = sleep_threshold_ * sleep_threshold_;
    // An island sleeps iff it has an edge and every dynamic member is slow
    // (pre-integration speed — the gravity kick of THIS step must not keep
    // a settled pile awake; the snapshot speeds already include it).
    for (std::uint32_t i = 0; i < n; ++i) {
      if (bodies_[i].inverse_mass <= 0.0f) continue;  // static: no state
      if (island_has_edge_[find_root(i)] == 0U) continue;  // free flight
      const PreStep& pre = pre_step_[i];
      const float speed_sq =
          pre.vx * pre.vx + pre.vy * pre.vy + pre.vz * pre.vz;
      // The same threshold doubles as the spin gate (rad/s): a body
      // spinning in place must not sleep. Zero-spin bodies (all history)
      // evaluate 0 < thr exactly as before.
      const float spin_sq = pre.wx * pre.wx + pre.wy * pre.wy + pre.wz * pre.wz;
      if (speed_sq < thr_sq && spin_sq < thr_sq) {

        sleeping_flags_[i] = 1U;
        ++sleeping_count_;
      }
    }
  }

  //! Freeze sleeping members at their pre-integration state (exact stored
  //! pose, never analytic undo). Runs after the solve loops, so any work
  //! the passes applied to a sleeper is discarded, and events already
  //! emitted stay a pure function of detection.
  //! Spin side effect of an applied impulse: dW = (r x J) / I. Linear
  //! motion is never touched here, so the historical v/p instruction
  //! sequence is bit-identical with or without torque. Sleepers skip:
  //! they are restored to pre-step spin below.
  void apply_torque(std::uint32_t id, float jx, float jy, float jz, float rx,
                    float ry, float rz) noexcept {
    if (id >= bodies_.size()) return;
    PhysicsBody& b = bodies_[id];
    if (b.inverse_mass <= 0.0F) return;
    if (is_asleep(id)) return;
    const float inertia = body_inertia(b);
    if (inertia <= 0.0F) return;
    b.angular_velocity[0] += (ry * jz - rz * jy) / inertia;
    b.angular_velocity[1] += (rz * jx - rx * jz) / inertia;
    b.angular_velocity[2] += (rx * jy - ry * jx) / inertia;
  }

  void apply_sleep_restore() noexcept {
    if (sleeping_count_ == 0) return;
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
      if (sleeping_flags_[i] == 0U) continue;
      if (bodies_[i].inverse_mass <= 0.0f) continue;  // static: untouched
      const PreStep& pre = pre_step_[i];
      PhysicsBody& b = bodies_[i];
      b.position[0] = pre.px;
      b.position[1] = pre.py;
      b.position[2] = pre.pz;
      b.velocity[0] = pre.vx;
      b.velocity[1] = pre.vy;
      b.velocity[2] = pre.vz;
      b.orientation[0] = pre.qx;
      b.orientation[1] = pre.qy;
      b.orientation[2] = pre.qz;
      b.orientation[3] = pre.qw;
      b.angular_velocity[0] = pre.wx;
      b.angular_velocity[1] = pre.wy;
      b.angular_velocity[2] = pre.wz;
    }
  }

  std::vector<PhysicsBody> bodies_;
  //! Per-step scratch, kept as members so the steady-state step allocates
  //! nothing after the first frame at a given body count.
  std::vector<PhysicsBody> check_saved_{};
  std::vector<SolveSnapshot> solve_snapshot_{};
  std::vector<ContactSolve> solve_contacts_{};
  std::vector<float> solve_lambda_{};  //!< Trial 638: per-contact XPBD lambda.
  std::vector<float> joint_lambda_{};  //!< Trial 638: per-joint XPBD lambda.
  //! Uniform-grid scratch. Bodies are bucketed in index order, so each bucket
  //! is already ascending and needs no sort.
  struct Cell {
    std::int64_t x{0};
    std::int64_t y{0};
    std::int64_t z{0};
    [[nodiscard]] bool operator==(const Cell& o) const noexcept {
      return x == o.x && y == o.y && z == o.z;
    }
  };
  //! Swept cell range for one body (speculative mode): the integer cells
  //! intersecting [min(start, end) - bound, max(start, end) + bound].
  struct CellRange {
    std::int64_t lox{0}, hix{0};
    std::int64_t loy{0}, hiy{0};
    std::int64_t loz{0}, hiz{0};
  };
  //! Flat open-addressing cell table keyed by a golden-ratio (Knuth MMIX
  //! 2^64/phi) multiplicative mix with linear probing (trial #608,
  //! ADR-004). Same occupancy and same i-ascending 27-neighbourhood walk as
  //! the unordered_map it replaced, so the candidate pair SET is identical
  //! and the trailing sort in rebuild_candidate_pairs() yields byte-equal
  //! output; only hash + table changed. Size is 4x the body count (power of
  //! two), so load never exceeds 25% and probing stays flat (measured max
  //! 13 at 16k bodies, mean ~1.3). Scratch is kept as a member: slots and
  //! their member vectors persist across frames, so the steady-state step
  //! allocates nothing after the first frame at a given body count.
  struct BroadphaseGrid {
    struct Slot {
      Cell key{};
      std::vector<std::uint32_t> members;
      bool occupied{false};
    };
    std::vector<Slot> slots;
    std::vector<std::uint32_t> used;  //!< slot indices touched this frame
    unsigned shift{0};
    static std::uint64_t mix_cell(const Cell& c) noexcept {
      constexpr std::uint64_t kPhi = 11400714819323198485ULL;  // 2^64 / phi
      std::uint64_t h = static_cast<std::uint64_t>(c.x) * kPhi;
      h ^= static_cast<std::uint64_t>(c.y) * kPhi + 0x9E3779B97F4A7C15ULL +
           (h << 6) + (h >> 2);
      h ^= static_cast<std::uint64_t>(c.z) * kPhi + 0x9E3779B97F4A7C15ULL +
           (h << 6) + (h >> 2);
      h *= kPhi;
      return h;
    }
    void reserve_for(std::size_t bodies) {
      std::size_t want = bodies * 4U + 16U;
      std::size_t m = 1;
      while (m < want) m <<= 1;
      if (m == slots.size()) return;
      slots.clear();
      slots.resize(m);
      used.clear();
      used.reserve(bodies + 1U);
      shift = static_cast<unsigned>(64 - __builtin_ctzll(m));
    }
    void clear_frame() noexcept {
      for (const std::uint32_t idx : used) {
        slots[idx].members.clear();
        slots[idx].occupied = false;
      }
      used.clear();
    }
    Slot& insert(const Cell& key) noexcept {
      const std::uint64_t h = mix_cell(key);
      std::size_t idx = h >> shift;
      for (;;) {
        Slot& s = slots[idx];
        if (!s.occupied) {
          s.occupied = true;
          s.key = key;
          used.push_back(static_cast<std::uint32_t>(idx));
          return s;
        }
        if (s.key == key) return s;
        idx = (idx + 1U) & (slots.size() - 1U);
      }
    }
    [[nodiscard]] const Slot* find(const Cell& key) const noexcept {
      const std::uint64_t h = mix_cell(key);
      std::size_t idx = h >> shift;
      for (;;) {
        const Slot& s = slots[idx];
        if (!s.occupied) return nullptr;
        if (s.key == key) return &s;
        idx = (idx + 1U) & (slots.size() - 1U);
      }
    }
  };
  bool force_all_pairs_{false};
  BroadphaseGrid grid_{};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> candidate_pairs_{};
  std::vector<ContactEvent> contacts_;
  float gravity_;
  std::uint32_t solver_iterations_{1};  //!< Positional-correction passes.
  std::uint32_t substeps_{1};           //!< Integration+solve substeps per step.
  float contact_compliance_{0.0f};      //!< XPBD alpha, 0 = stiff (default).
  bool determinism_check_{false};       //!< Double-step validator (debug).
  std::uint64_t determinism_violations_{0};  //!< Mismatches seen while on.
  float restitution_threshold_{0.0f};   //!< Bounce cutoff speed, 0 = always.
  float sleep_threshold_{0.0f};         //!< Island sleep speed, 0 = never.
  bool speculative_contacts_{true};     //!< Swept TOI contacts, 1 = on.
  //! Minimum penetration for a contact to join islands. Dust is ~1e-8,
  //! genuine rest ~1e-4; see the build_islands comment.
  static constexpr float kSleepEdgeMinPenetration = 1e-5F;
  //! Island/sleeper scratch (derived per step, never snapshotted).
  std::vector<PreStep> pre_step_{};
  std::vector<std::uint8_t> grounded_{};
  std::vector<float> grounded_depth_{};
  std::vector<float> grounded_approach_{};  //!< VFP ground approach speeds
  bool velocity_from_position_{false};      //!< XPBD velocity update (opt-in)
  std::vector<DistanceJoint> joints_{};     //!< insertion-order constraints
  std::vector<std::uint32_t> island_parent_{};
  std::vector<std::uint8_t> island_has_edge_{};
  std::vector<std::uint8_t> sleeping_flags_{};
  std::uint32_t sleeping_count_{0};
};

}  // namespace warploom::physics
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).
#ifndef WARPLOOM_COMPAT_PHYSICS_NS
#define WARPLOOM_COMPAT_PHYSICS_NS
namespace omnicpp::physics {
    using namespace ::warploom::physics;
}
#endif  // WARPLOOM_COMPAT_PHYSICS_NS
