# llvmpipe JIT segfault: viewport + 2 tests (environment issue, not engine)
- Two tests and the live viewport crash inside Mesa's raster JIT; the only
| mesa 3:26.2.2-2 (pre-Sept-19 downgrade, local pacman cache) | segfault; **not a recent regression** |
| `DISPLAY=:0` (Xwayland) vs `DISPLAY=:99` (Xvfb, no GPU compositor) | segfault; display server irrelevant |
| `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` on/off | segfault; validation not involved |
1. ~~Reboot~~ **DONE**; real NVIDIA driver verified (563/563 on hardware).
   fully sufficient**; the lavapipe suite, control-protocol E2E over unix