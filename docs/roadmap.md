follows the project's verification discipline (bottom of this page); a
## Phase A: substrate (done)
- [x] **A1 shaders are a build product**; moved out of `tests/` to a
- [x] **A2 the clone-and-build gate**; `cmake --preset default` aborted on a
- [x] **A3 identity debt closed**; 339 include sites re-badged off the 58
- [x] **A4 validation is actually clean**; three defects the status page
- [x] **A5 quality gates run**; the lint and format targets passed
- [x] **A6 dependencies**; six declared packages, one real. Five phantom
- [x] **A7 packaging and release**; `cpack` aborted on a template
- [x] **A8 CI builds the viewport**; every job previously set
## Phase B: one render path
pass, so a scene mixing a rigged actor with static geometry; which is what
the application does; could not be expressed. `ScenePbrObject::skinned`
- [x] **B1a shadow pre-pass through `record_shadow_pre_pass`**; the
- [x] **B1b lit pass**; already delegated to `record_pbr_scene`; the only
- [x] **B4 GPU-driven path**; `record_gpu_driven_cull` and
- [x] **B2 compose chain runs on the render graph**; `record_compose_chain`
- [x] **B3 HDR target + post chain**; the renderer now owns an HDR
- [x] **B3b frame capture through the compose chain**; DONE. The viewport
        1. DONE; `FullscreenPass`, `record_fullscreen_draw`,
           (vulkan_compose_chain.hpp/.cpp); allocator, descriptor manager,
        3. DONE; see 2. The renderer holds one `std::unique_ptr
- [x] **B5 H-Z depth source**; the reduction read
- [x] **B5b an occlusion consumer**; DONE (increments 1+2 below). Was: the
      that do read occlusion do not fit -
      monotonically increasing word offsets; exactly the ABI
      and a `WARPLOOM_NO_MANNEQUIN=1` selector; the bundled asset always
      change versus occlusion off; tiles are populated and culls fire; with
      Remaining: nothing blocking; an occlusion-heavy demo view is a
- [x] **B6 no duplicated Vulkan in the app**; raw `vkCmd*` calls in the
      reading of the gate; no `vkCmd` at all outside `modules/render` -
## Phase C: one data model, one tick
- [x] **C1 document → ECS projection**; `DocumentProjection`
      channels. The document stays authoritative; undo/redo/save all still go
      through it; and `project()` is what carries edits into the runtime store.
- [x] **C2 one `tick(FrameInput)`**; `EditorSession::tick` runs
      correctly under the host; with a comment in `headless_host.cpp`
- [x] **C3 physics in the tick and in the protocol**; the session owns a
      integration; asserted by stepping two sessions 20 frames and requiring
- [x] **C4 the test-local ECS bridge is gone**; `test_physics_ecs_bridge`
      property lives; `position_fingerprint` over a thousand bodies needs no
      bridge; and a new case runs the same thousand bodies end to end through
      so nothing regressed; it got marginally faster by dropping the per-step
## Phase D: the three thin products
- [ ] **D1 real physics**; extract `warploom-physics`; more shapes, joints,
      1.05x), and then found *not* result-preserving at N=4000 and N=8000 -
      them; and the all-pairs loop would have caught those, since it re-reads
- [x] **D1a order-independent contact detection**; step 3 now detects against
      measured; 200, 600, 1200, 1000, 4000 and 8000 bodies; where the
      rather than by rebuilding with the threshold edited by hand -
      increments; shrinking the increment itself, complementary to the
      passes; a deep-stacked pile is exactly where single-pass sink shows.
      (docs/research/physics-solver.md; Small Steps SIGGRAPH 2021, MGPBD
      over the frozen contact graph; Jolt's per-step rebuild pattern);
      Remaining: rotation, joints; each a design arc, sized in
- [x] **D2a interpolated, eased track evaluation**; `TimelineClip::evaluate_at`
      result *exactly*; a replay of an existing recording has to re-run
- [x] **D2b sub-frame tick position reaches playback**; `FrameInput` carries a
      advancing clip playback; the physics stage checked `paused` and the
      existing three-element `tick({frame, dt, paused})` call site; `true`
- [x] **O1 structured diagnostics**; `warploom/core/diagnostics.hpp`. One
- [x] **O2 GPU timing actually resolves, per pass**; it never had. Capability
      written; the H-Z boundary when occlusion is off, and any boundary for a
      now carries real per-frame values; 1991 distinct readings across a run,
      ~69us/frame; and still reports `-1` before the first resolve instead of
- [x] **Intermittent segfault in the threaded scheduler test**; FIXED.
      structurally; the old ordering was undefined by the standard, the new
- [ ] **D3 game depth**; ECS as the app's data model, scene management,
      strict scanner (machine-written shape only; the M5 engine-written
      module to a node type named "script:<module>"; Number in/out pins sized
      a wall clock, so replay re-simulation reproduces the sequence exactly -
      leaves outputs 0; the same failure shape as a math node given garbage.
      outputs through the graph; the full path a gameplay module takes.
## Phase E: release
- [x] **E1 render modes as protocol commands**; DONE (protocol v1.9).
- [ ] **E2 R1–R4; exposure/bloom controls DONE (see below); remaining:
      `set_exposure` (protocol v1.9) is live end-to-end; validated by
      every frame; the preserve is now explicit in the chain), plus a live
      reflections/AO (R2), skinned gd scenes (R3); spec rewritten from the
      passes record; that inverts the bloom-toggle revert's root cause by
      PT"; pause, accumulate K frames into the rgba32f accum image via the
      existing deterministic PCG contract, tonemap from accum; deterministic
      REVERTED attempt; runtime bloom toggle. The full-recreate path
      BLACK with a device leak; the light path's rebuild sequence does not
- [ ] **E3 hardware CI runner**; every RT test skips on lavapipe, so the
- [x] **E4 warning debt, then `-Werror`**; DONE. All four presets build
- [x] **E5 formatting**; DONE. The tree already conforms: clang-format
- [ ] **E6 P1–P4**; WASM leg, native Wayland surface, adoption
1. **Clone-and-build**; a fresh clone reaches a working viewport with no
2. **Modules real**; the viewport and tests consume installed packages; no
3. **Demo story works**; record, replay, scrub, save/load, proven over the
4. **Signature render feature**; R1 path tracing, toggleable in the
   viewport. **Not met (R1)**; but the R1 spec now exists
   PT shaders are test-proven; R1 is a viewport integration with a written