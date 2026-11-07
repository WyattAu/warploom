# Wayland backend research: native viewport presentation (E6)

Status: adopted for the viewport slice (loop69). The engine half
(`create_platform_surface` Wayland branch, loop68) is already landed;
this note covers the client half: xdg-shell window + swapchain present.

## Sources (all vendor documentation, labelled as such — no technique borrowed)

- `stable/xdg-shell/xdg-shell.xml`, wayland-protocols 1.50
  (gitlab.freedesktop.org/wayland/wayland-protocols, MIT-licensed;
  vendored under `examples/viewport/wayland-protocol/` with attribution).
  The protocol spec is the XML plus its inline doc comments: the
  `xdg_wm_base` → `xdg_surface` → `xdg_toplevel` object chain, the
  configure handshake (`xdg_surface.configure` ack + `wl_surface.commit`
  before the first frame), and `xdg_toplevel.close` as the only exit
  signal. Stable since 2018; kwin_wayland implements it.
- `VK_KHR_wayland_surface` (Khronos Vulkan registry, vendor
  documentation): `vkCreateWaylandSurfaceKHR` stores `(wl_display,
  wl_surface)`; presentation support is per queue family
  (`vkGetPhysicalDeviceWaylandPresentationSupportKHR`). Proven live in
  loop68 (`VulkanWayland.CreateSurfaceOnLiveCompositor`): at least one
  queue family on this machine's NVIDIA driver presents to kwin_wayland.
- Wayland core protocol (`wayland.xml`, same vendor source): the
  registry/roundtrip pattern the proof test already uses; event
  dispatch is `wl_display_dispatch_pending`/`dispatch` around the frame
  loop, same position the XCB poll occupies today.

## Claim being borrowed

None. This is client bring-up against a stable published protocol,
not a technique. The only design decision is the slice boundary (see
below), which is project judgment, not literature.

## Novelty assessment

Nothing novel. Every Wayland client (GLFW, SDL, Weston clients) does
exactly this chain. Combination notes: the viewport keeps its
deterministic frame loop (control-socket stepping already decouples
ticks from wall clock and from the XCB poll); the Wayland dispatch
occupies the same slot, so no tick ever reads compositor timing.

## Why it fits here, and what it costs

- Fits: the swapchain/renderer/capture stack is platform-agnostic
  (takes `VkSurfaceKHR`); only window creation, the surface wrap
  (landed), event dispatch, and resize-on-configure are platform code.
  The control socket + scripted input already make runs reproducible
  without human input, so a backend without seat input is still fully
  provable (stepped captures compared pixel-equal across backends).
- Costs: one vendored protocol XML (~64 KB, MIT, attributed) plus
  build-time `wayland-scanner` generation, viewport-only. Engine
  untouched. Seat input (keyboard/pointer via xkbcommon + `wl_seat`)
  is deliberately the NEXT slice, not this one: without it the
  Wayland viewport runs scripted/control-driven sessions only.
- What was measured: loop68 numbers (creation + present-support on
  this machine); loop69 must measure present-path frames + 0
  validation diagnostics + pixel-equality with the XCB backend.

## Slice boundary (loop69 vs loop70 vs loop71)

- Loop69: display → registry → compositor + xdg_wm_base → wl_surface
  → xdg toplevel → configure handshake → engine Wayland surface →
  swapchain → existing frame loop; `xdg_toplevel.close` exits;
  `xdg_surface.configure` (size) routes to the existing
  `apply_resize`. No `wl_seat`, no xkbcommon, no pointer/keyboard:
  interactive use stays on XCB until loop70.
- Loop70 (as landed): keyboard-only seat parity (wl_seat v1 bind,
  keymap/modifiers via xkbcommon, XKB_V1 fd keymap, ESC closes).
  X11 codes are evdev + 8, so the shared dispatch is fed unchanged;
  printables come from the keysym (layout-correct, unlike the XCB
  tables). Pointer motion + buttons explicitly deferred to loop71.
  Live key-delivery e2e deferred (environmental: live-session focus
  prevention, saturated GPU) — setup handshake proven live, mappings
  unit-pinned and sabotage-checked, XCB post-extraction captures
  byte-identical. The input-script replay comparison stays the
  loop71 proof.
- Loop71 (as landed): pointer motion + buttons parity (wl_pointer
  object, Linux BTN_* to XCB 1/2/3, wl_fixed 24.8 converted at one
  site). XCB motion/press/release blocks extracted verbatim into
  shared helpers (249-line reconstruction proof holds); enter syncs
  position without feeding the translator, leave is a no-op, wheel
  axis stays null (needs seat v5+ discrete policy — input-hardening).
  Setup proven live (seat v10 bound at v1, caps keyboard+pointer,
  get_keyboard AND get_pointer issued, zero protocol errors, clean
  teardown). Delivery e2e still gated (focus + renderer init under
  saturated VRAM); the input-script replay comparison stays the
  deferred proof, runnable when VRAM frees.
- Input-hardening (same session): seat bind bumped v1 -> v8 for
  axis_value120 wheel detents (v2 name already handled; axis_discrete
  is deprecated since v8 and never sent to v8+ clients, so no
  double-count hazard). Detents feed the translator as XCB-style 4/5
  presses; smooth partials truncate toward zero (documented
  no-zoom, never mis-zoom); sign from the protocol text (value120
  shares motion coordinates, y-down). Pure mapping unit-pinned,
  sabotage-checked both directions; v8 bind proven live with zero
  protocol errors.
