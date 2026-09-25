# llvmpipe JIT segfault — viewport + 2 tests (environment issue, not engine)

## Status (2026-09-25, updated)

**Resolution: the machine was rebooted; the NVIDIA driver (615.71.09, kernel
7.2.6) is healthy again. All 563 tests pass on hardware with 0 skips, and the
live viewport runs normally. The material below is retained as the record of
the llvmpipe-side investigation, which remains accurate for software-Vulkan
CI runs.**

The NVIDIA driver was broken at the time (kernel module 610.57.04 in memory vs
615.71.09 userspace after the 2026-09-19 `pacman -Syu`). While waiting for the
reboot, all verification moved to lavapipe (Mesa software Vulkan). Result:

- **Full test suite on lavapipe: 560 tests, 555 pass, 5 documented skips, 0 failures.**
- Two tests and the live viewport crash inside Mesa's raster JIT — the only
  paths in the project that exercise llvmpipe's fragment rasterization on
  real (JIT-compiled) shaders at scale.

## Crash signature

```
Thread N "llvmpipe-N" received signal SIGSEGV
#0  0x00007ffff6318fe4 in ?? ()      <- anonymous 8KB r-xp mapping (JIT code)
#1  0x0000000000000000 in ?? ()      <- return address NULL: trampoline frame
```

- Address varies with ASLR slide but is always at the same fixed offset inside
  an anonymous r-xp mapping = JIT-emitted raster code, not engine code.
- All llvmpipe worker threads sit at the same faulting PC.
- The offset differs per process (gdb re-JITs), so it is data-dependent
  codegen crashing, not a fixed bad instruction.

## Isolation matrix (all repro the crash)

| Configuration | Result |
|---|---|
| mesa 3:26.2.3-1 (current) | segfault |
| mesa 3:26.2.2-2 (pre-Sept-19 downgrade, local pacman cache) | segfault — **not a recent regression** |
| `LP_NUM_THREADS=1` (single-threaded JIT/raster) | segfault |
| `LP_NATIVE_VECTOR_WIDTH=128` (different SIMD codegen) | segfault |
| `GALLIUM_DRIVER=softpipe` (softpipe is GL-only; Vulkan still llvmpipe) | segfault |
| `DISPLAY=:0` (Xwayland) vs `DISPLAY=:99` (Xvfb, no GPU compositor) | segfault — display server irrelevant |
| `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` on/off | segfault — validation not involved |
| `OMNICPP_LEGACY_LIGHTING=1 OMNICPP_NO_SHADOW=1` (minimal shader stack) | segfault |
| `OMNICPP_NODE_EDITOR=0` (UI overlay off) | segfault |
| lavapipe compute/cull/IBL/RT path tests (46/46 VulkanHardware compute-side) | **pass** |

## Conclusion

Defect in the CachyOS `mesa 3:26.2.x` + bundled LLVM 22.1.8 raster JIT on this
CPU, bounded from every side we can control. Engine-side code validates clean
(555/560 green, incl. all compute paths). Not fixable from the project.

## Practical workarounds

1. ~~Reboot~~ **DONE** — real NVIDIA driver verified (563/563 on hardware).
2. On machines without a working hardware driver: **headless verification is
   fully sufficient** — the lavapipe suite, control-protocol E2E over unix
   sockets, and byte-level readback tests exercise everything except live
   windowed presentation (the viewport + 2 raster tests hit the JIT bug).
3. Optionally report upstream to CachyOS/Mesa with the isolation matrix above.

## If mesa gets pinned / downgraded again

The pre-upgrade package is in the local cache and installs cleanly, but was
verified to make **no difference**:
`sudo pacman -U /var/cache/pacman/pkg/mesa-3:26.2.2-2-x86_64.pkg.tar.zst`
