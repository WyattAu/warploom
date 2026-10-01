# warploom-asset — S3.5 gate

Status: GATE (extraction not started; this document is the audit + contract)

## 1. What moves

The asset import layer of the monolith: 6 public headers under
`include/engine/asset/` and 6 translation units (+1 TU-private header)
under `src/engine/asset/`. After S3.5 they live in `modules/asset/` as
`warploom_asset` (exported `Warploom::asset`, project `WarploomAsset`,
package `WarploomAsset`), mirroring the module pattern exactly (SHARED,
PIE, `Warploom::asset` alias, `EXPORT_NAME asset`, own Config.cmake.in +
version file, GLOB header allowlist, compat forwarders
`include/engine/asset/X.hpp` → one-line include of `warploom/asset/X.hpp`).

| public header | owns |
|---|---|
| gltf_importer.hpp | glTF/GLB mesh + scene import |
| gltf_animation.hpp | skeletal animation import |
| image_decode.hpp | shared image decoding entry |
| png_decoder.hpp | strict self-contained PNG decoder |
| jpeg_decoder.hpp | strict self-contained JPEG decoder |
| ktx2_decoder.hpp | KTX2 transcode-safe container decode |

Plus `src/…/gltf_json.hpp` — internal (strict RFC 8259 parser, base64/
data-URI, accessor decoding), included by relative path from TUs only;
moves with the module, NOT installed, not part of the frozen surface.

Names stay `omnicpp::asset` this phase — S3.5 is an include-path and
packaging extraction only (same policy as S3; identifier migration is
S5-B scope, decided in the roadmap amendment).

## 2. Dependency audit (measured)

### 2.1 What asset consumes

- `engine/core/deterministic_runtime.hpp` ×4 in public headers (→
  `warploom/core/deterministic_runtime.hpp` after the move; core is the
  ONLY module dependency — zero render/ui/editor leakage, grep-verified).
- `RuntimeError` (core) in TUs.
- Cross-includes inside the layer: image_decode ← png/jpeg/ktx2 TUs,
  gltf_importer ← glf_animation header. All internal.
- External codec libraries: **none**. PNG/JPEG/KTX2 decoders are strict,
  self-contained, written in-tree ("Strict self-contained PNG decoder",
  see file headers). No find_package / pkg_check / vendored-third-party
  surface — the module has no Vulkan-style plumbing problem at all.
- `OMNICPP_HAS_VULKAN` etc.: not referenced by any asset header/TU.

### 2.2 What consumes asset (include sites)

| consumer | usage | mechanism after S3.5 |
|---|---|---|
| `examples/viewport/main.cpp` | gltf_importer + glf_animation, `omnicpp::asset` spellings | forwarders (zero edits) |
| 8 `tests/unit/test_*.cpp` TUs (gltf_animation, gltf_importer, gltf_scene, gpu_mannequin, jpeg, ktx2, png_decoder, png_texture) | decoders + importers | forwarders (zero edits) |

Grep across `src include examples tests modules tools`: **no other
consumers** — no module includes asset headers, no engine-layer TU does.
9 total external include sites, all kept compiling via forwarders.

### 2.3 Test placement decision (recorded)

The 8 asset test TUs **stay in the root suite** (via forwarders), and the
module gets **no standalone gtest target** this phase — same decision as
render. Rationale: the tests are fixture-driven
(`OMNICPP_TEST_ASSET_DIR` → `assets/models`, superproject-side define +
files; gpu_mannequin additionally exercises the GPU path), so in-module
placement would either duplicate fixture plumbing or silently split the
suite. The full root suite compiles them against the module through the
forwarders, which is the real coverage claim; revisit if the module
gains headless-only logic.

### 2.4 What stays in the monolith

After S3.5, `src/engine/` contains **no TUs at all** — `omnicpp_runtime`
becomes an empty shell. Its disposition is S5-A scope (delete it when
the aggregate lands); S3.5 keeps the target existing but with zero
sources, linking the five modules PUBLIC so every existing consumer
target keeps linking unchanged.

## 3. Mechanical plan

1. `git mv` 6 headers → `modules/asset/include/warploom/asset/`; 6 TUs +
   gltf_json.hpp → `modules/asset/src/warploom/asset/`. Rewrite
   `engine/asset/X.hpp` → `warploom/asset/X.hpp` and
   `engine/core/Y.hpp` → `warploom/core/Y.hpp` via sed (line-oriented;
   per-file line counts asserted unchanged against HEAD — the S2-B/S3
   discipline). gltf_json.hpp keeps its relative include.
2. `modules/asset/CMakeLists.txt` per the module pattern: SHARED + PIE +
   cxx_std_23, `target_link_libraries(warploom_asset PUBLIC
   Warploom::core Threads::Threads)`, GLOB allowlist, install + export
   + Config + version file. No Vulkan/xcb/platform plumbing (§2.1).
3. `cmake/WarploomAssetConfig.cmake.in`: `find_dependency(WarploomCore)`
   + `find_dependency(Threads)`.
4. Forwarders: 6 one-liners under `include/engine/asset/` (S3.5 comment).
5. `src/engine/CMakeLists.txt`: drop the 6 asset TUs (no sources left);
   keep `omnicpp_runtime` as a no-source SHARED placeholder linking
   `Warploom::ui Warploom::core Warploom::editor Warploom::render
   Warploom::asset` PUBLIC, alias block extended with
   `omnicpp_asset ALIAS warploom_asset`. Root CMakeLists adds
   `add_subdirectory(modules/assets)` — **before** `src/engine`
   (plain-name resolution; ORDER MATTERS — same as S3).

## 4. Correctness risks and mitigations

1. **Empty SHARED library** — CMake may warn/refuse a SHARED target with
   no sources. Mitigation: pass at least the target's link interface
   through `$<BUILD_INTERFACE:…>`-free plain sources-free form
   (`add_library(omnicpp_runtime SHARED)` with no sources is legal since
   CMake 3.11 for imported-style stubs; if the linker dislikes it, fall
   back to INTERFACE — S5-A deletes the target anyway, and no consumer
   links `omnicpp_runtime` by artifact name in a way INTERFACE breaks).
2. **Fixture paths in tests** — `OMNICPP_TEST_ASSET_DIR` is
   superproject-side; unaffected by the move (tests keep compiling via
   forwarders; §2.3).
3. **Namespace stays** — no `warploom::asset` identifiers yet; zero
   S2-B doubled-name risk. `omnicpp::asset` spellings untouched.
4. **Link topology** — asset symbols move from `libomnicpp_runtime.so`
   to `libwarploom_asset.so`; runtime links it PUBLIC, so the 42-TU
   suite and viewport keep resolving symbols transitively. Single-ABI
   contract unchanged (one DSO per module).

## 5. Verification plan (machine-checked)

1. Four-leg matrix (vulkan-validation, headless-debug-clang, tsan,
   asan-ubsan): full 6-suite ctest each.
2. Fresh install tree (`rm -rf /tmp/editor_install` first — headers
   move); `WarploomAsset` package config present.
3. Consumers re-proofed fresh (clean build dirs): CORE/UI/EDITOR/RENDER
   consumers + new `/tmp/asset_consumer` (ASSET_CONSUMER_OK) proving
   `find_package(WarploomAsset CONFIG REQUIRED)` standalone with an
   `omnicpp::asset` spelling (exercises the compat path; decodes a tiny
   in-source PNG byte string and asserts dimensions — no fixture files
   needed).
4. 64/64 live proofs (systemd hosts, positional socket path).
5. `python3 scripts/check_docs_links.py` before doc commits.
6. Roadmap S3.5 `[x]` only after all of the above.

## 6. Non-goals

- No `warploom::asset` identifier migration (S5-B).
- No new codecs, no external library adoption (stb_image etc. — the
  self-contained decoders are a determinism feature, not a gap).
- No module-internal gtest suite (§2.3).
- No `omnicpp_runtime` deletion (S5-A; the target survives as a shell).
