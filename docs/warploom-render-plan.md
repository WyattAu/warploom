# warploom-render: S3 gate
Names stay `omnicpp::render` this phase; S3 is an **include-path and
| job_system.hpp | vulkan_parallel_recorder header (S2-B: real include replaced the fwd-decl; a real member declaration hides directive-injected types) | (already in core) |
`modules/core/include/warploom/core/*` post-S2; **no core change
(never `engine/core/*` forwarders; S4 banked lesson).
| engine glue TUs (main.cpp, asset) |; | none (link-time only) |
`src/engine/asset/*` (6 TUs) stays; asset is render-adjacent but
   `// ====` doc dividers; S2-B lesson; line count must never shrink).
   (CMake resolves plain names at call time; ORDER MATTERS; editor/core
   22 TUs for zero assertion gain; decision recorded, revisit if the
1. **PUBLIC compile definition loss**; `OMNICPP_HAS_VULKAN` gates
2. **Vulkan optional**; headless-debug-clang leg is the canary: if
   compile; the graceful path lives in TUs that already guard on
3. **Link topology change**; render symbols move from
4. **Namespace stays**; no `warploom::render` identifiers yet; zero
5. **Forwarder-only consumer churn**; 44 include sites keep their
   `asan-ubsan`; full 6-suite ctest (`timeout 850 ctest … -j4`).
   /tmp/editor_install` (rm -rf the old one first; headers change).
   G3B_CONSUMER_OK); rebuild clean (`rm -rf` their build dirs first)