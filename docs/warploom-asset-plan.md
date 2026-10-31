# warploom-asset: S3.5 gate
> needed immediately; CMake rejects `add_library(omnicpp_runtime SHARED)`
Plus `src/…/gltf_json.hpp`; internal (strict RFC 8259 parser, base64/
Names stay `omnicpp::asset` this phase; S3.5 is an include-path and
  ONLY module dependency; zero render/ui/editor leakage, grep-verified).
  surface; the module has no Vulkan-style plumbing problem at all.
consumers**; no module includes asset headers, no engine-layer TU does.
module gets **no standalone gtest target** this phase; same decision as
After S3.5, `src/engine/` contains **no TUs at all**; `omnicpp_runtime`
   per-file line counts asserted unchanged against HEAD; the S2-B/S3
   `add_subdirectory(modules/asset)`; **before** `src/engine`
   (plain-name resolution; ORDER MATTERS; same as S3).
1. **Empty SHARED library**; CMake may warn/refuse a SHARED target with
   back to INTERFACE; S5-A deletes the target anyway, and no consumer
2. **Fixture paths in tests**; `OMNICPP_TEST_ASSET_DIR` is
3. **Namespace stays**; no `warploom::asset` identifiers yet; zero
4. **Link topology**; asset symbols move from `libomnicpp_runtime.so`
2. Fresh install tree (`rm -rf /tmp/editor_install` first; headers
   in-source PNG byte string and asserts dimensions; no fixture files
- No new codecs, no external library adoption (stb_image etc.; the