# Loop 27: dynamic rendering for fullscreen passes (Blade stage 0)

Status: stages 0-2 landed 2026-11-01 on the RTX 2060. Stage 0 (this loop)
enables the feature bit and proves begin/end on-device. Stage 1 migrates
the compose bloom downsample; stage 2 the tonemap (present image threaded
per call, modeled as caller-owned history). Later stages move the
remaining fullscreen passes (PT blit, sky, UI) off baked render passes;
each stage lands with its own re-proof.

## Source

Vulkan 1.3 core specification, sections on dynamic rendering
(`vkCmdBeginRendering` / `VkRenderingInfo`); no extension required.
Vendor documentation (spec), not evidence — the evidence is
`DynamicRendering.ClearOnlyPassWritesAttachment` on the RTX 2060
(recorded 2026-11-01) plus the same binary skipping-with-reason where
the feature is absent (lavapipe advertises it, so CI executes it).

## Claim being borrowed

A fullscreen pass does not need a `VkRenderPass`/`VkFramebuffer` pair:
the attachment (view, layout, load/store op, clear value) is declared
inline per recording. Passes un-bake; pipeline objects stay valid
(dynamic rendering pipelines are created without a render pass, so the
migration also touches pipeline creation, not just recording).

## Novelty assessment

Standard. Dynamic rendering is core since Vulkan 1.3 (2022); every
major engine ships it. Nothing here is novel; this is adoption
engineering. Staging (flag first, passes later) is the only judgment
call, and it exists so each pass keeps its golden hash green.

## Why it fits here

The compose chain, bloom, PT blit and UI passes all render into
plan-derived transient images whose extents can change (resize
rebuilds). Baked render passes + framebuffers must be recreated per
extent; dynamic rendering records against views directly, which removes
an entire class of resize-rebuild code (the capture-stack rebuild that
caused the B4 device-loss is the shape of bug this retires). It is also
the prerequisite for GENERAL-layout PT/bloom layering: with no baked
pass, a pass can consume whatever layout the previous stage produced.

## Cost

- Device side: one feature bit (`dynamicRendering`), core in 1.3, no
  extension. Devices below 1.3 skip the later stages; the baked path
  stays until every migrated pass proves its golden hash.
- Determinism: none. Dynamic rendering changes pass framing, not
  fragment math; the same shaders, same clear values, same order.
- Migration risk per pass: pipeline creation flags change
  (`VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT` is unrelated; the relevant
  change is `VkPipelineRenderingCreateInfo` in the pipeline pNext
  chain). Missing it is a validation error, not silent corruption —
  the layer catches it, which is why stage 0 asserts zero diagnostics.

## What was measured

Stage 0: feature negotiates on the RTX 2060 (`has_dynamic_rendering()`
true); the clear-only dynamic pass writes 64x64 red pixels read back
bit-exact with 0 validation errors and 0 warnings.

Stage 1 (2026-11-01, RTX 2060): the bloom downsample records via
`vkCmdBeginRendering` when `enable_dynamic_rendering` is requested and
advertised, baked otherwise. `ComposeBloomMigratesToDynamic` proves the
routing (bloom in dyn_passes, tonemap in passes), the node-list gate
(mixed nodes verify; a forged unknown-image sample fails it), the
fallback (dyn empty, two baked passes when unrequested), and a mixed-
graph execute with 0 diagnostics. `UnifiedFrameWithDynamicBloomRecordsClean`
drives the production unified frame (scene + dynamic bloom + tonemap)
through `VulkanRenderer`: non-clear readback, 0 errors, 0 warnings.
No frame-time claim yet; the per-config budgets re-measure when the
remaining passes migrate.

Stage 2 (2026-11-01, RTX 2060): the tonemap records via
`vkCmdBeginRendering` into the swapchain image whenever the framing is
negotiated and the caller supplies the present image/view (per-call
parameters through build/record/record_commands -- the renderer resolves
the swapchain image internally, headless callers pass their stand-in;
null keeps the baked path). The present image is a fifth plan kind
(kKindPresent): caller-owned history, planned but never created, exactly
like the external source. `UnifiedFrameWithDynamicBloomRecordsClean`
asserts golden parity (canonical hash 12508388375569283214, identical to
the baked golden) on frame one and a non-clear dynamic-bloom frame two,
0 diagnostics; a wrong-final-layout sabotage fails with VUIDs plus a hash
mismatch, then reverts. Baked callers (null present) are untouched: every
pre-existing golden hash still green.
