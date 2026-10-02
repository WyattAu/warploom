# warploom identity — S5-B gate

Status: GATE (migration not started; this document is the audit + contract)

## 0. The shim principle

Every old spelling keeps working through a shim until 0.1 ships; the
repo-internal code migrates first, the compatibility surface decays on
a documented schedule (drop defaults to post-0.1). Nothing published
exists yet, so the shim's job is to make the rename invisible to
anyone standing on any commit of this repo.

## 1. Census (measured)

### 1.1 Namespaces

| module | declarations | qualified spellings |
|---|---|---|
| render | `omnicpp::render` (+ `::render::depth` nested) | 94 + 2 self; 768 `omnicpp::core::` cross-refs |
| editor | `omnicpp::editor` (widgets) | 14 self; 7 `omnicpp::core::` |
| asset | `omnicpp::asset` (+ `::asset::gltf_detail` internal) | 24 self; 41 `omnicpp::core::` |

Target: `warploom::render` / `warploom::editor` / `warploom::asset`.
**Editor is a merge, not a move**: `warploom::editor` already exists
(core's session types, S2-B). Extension blocks merge — verified twice
(core's real `omnicpp::core` extension block pre-S2-B; the S2-B
using-directive scheme). The editor module's types join the same
namespace; the existing core compat footer
(`namespace omnicpp::editor { using namespace ::warploom::editor; }`,
guard `OMNICPP_COMPAT_EDITOR_NS`) remains THE compat directive for both
families — editor module headers repeat the identical guarded block so
each header is self-sufficient (same guard + same statement is safe:
include-once).

Cross-module spellings inside modules re-badge to **rooted**
`::warploom::core::…` (S2-B lesson: rooted spellings cannot double).

### 1.2 Build surface (code-visible defines + CMake + env)

- Compile definitions: `OMNICPP_HAS_VULKAN` (225), `OMNICPP_CONTRACT`
  (66 — the contract macro), `OMNICPP_VULKAN_TYPES_AVAILABLE` (45),
  `OMNICPP_HAS_QT_VULKAN` (19), test/app feature defines
  (`OMNICPP_TEST_*`, `OMNICPP_NO_SHADOW`, `OMNICPP_RT_MODE`,
  `OMNICPP_SCENE`, `OMNICPP_GPU_DRIVEN`, `OMNICPP_SPONZA`,
  `OMNICPP_NODE_EDITOR`, `OMNICPP_LEGACY_LIGHTING`), footer guards
  `OMNICPP_COMPAT_*_NS`.
- CMake options/vars: `OMNICPP_USE_VULKAN`, `OMNICPP_BUILD_ENGINE`,
  `OMNICPP_BUILD_TESTS`, `OMNICPP_LEGACY_ENGINE`,
  `OMNICPP_VULKAN_VALIDATION_TESTS`, `OMNICPP_USE_QT`,
  `OMNICPP_SHADER_DIR`/`OMNICPP_SHADER_COMPILER*`, fuzz/coverage/
  property-testing switches, `OMNICPP_PLATFORM_*`/`OMNICPP_ARCH_*`/
  `OMNICPP_COMPILER_*` (compiler-info plumbing).
- Binaries: `omnicpp_unit_tests`, `omnicpp_headless_host`,
  `omnicpp_viewport`, `omnicpp_runtime_only_tests`,
  `omnicpp_contract_violator`, `omnicpp_deterministic_runtime_benchmark`,
  module suites `warploom_{core,ui,editor}_tests` (already new-style).
- Env vars: `OMNICPP_RT_MODE`, `OMNICPP_SCENE`, `OMNICPP_GPU_DRIVEN`,
  `OMNICPP_SPONZA`, `OMNICPP_NO_SHADOW`, `OMNICPP_LEGACY_LIGHTING`,
  `OMNICPP_SHADER_DIR`.
- Package dirs: root project `OmniCppTemplate` (installs an
  `OmniCppTemplate` package config dir); modules already `Warploom*`.

### 1.3 ABI + strings

- Script-module C ABI: `omnicpp_module_abi` / `omnicpp_module_name` /
  `omnicpp_module_tick` (fixtures in `tests/unit/module_fixture/`).
- Contract diagnostic string `"omnicpp contract violation"`
  (asserted in `tests/unit/test_contract.cpp`).

## 2. Phases (each ends machine-verified; one commit per step)

### Phase 1a — `warploom::render` + `warploom::asset`

1. Module-internal sed (line-oriented; per-file line-count assertion vs
   HEAD + appended-footer arithmetic): declarations + self-spellings
   `omnicpp::render[::depth]` → `warploom::render[::depth]`;
   `omnicpp::asset` → `warploom::asset`; cross-refs → rooted
   `::warploom::core::`.
2. Footers appended at EOF of all 27 render + 6 asset public headers:
   per-namespace guarded using-directive
   (`OMNICPP_COMPAT_RENDER_NS` / `OMNICPP_COMPAT_ASSET_NS` — DISTINCT
   from core's guards; one shared guard would suppress later headers'
   directives). Depth: the render directive covers qualified lookup of
   `omnicpp::render::depth` ([namespace.qual]); no separate guard.
3. Zero consumer edits: 42 root-suite TUs + viewport + 8 asset TUs keep
   `omnicpp::` spellings and exercise the footers.

### Phase 1b — editor merge into `warploom::editor`

Same mechanics; the footers repeat core's exact guarded statement
(`OMNICPP_COMPAT_EDITOR_NS`) so both families resolve through one
directive. 14 self-spellings → `warploom::editor`; module tests
(in-tree, 39/39) re-badge with the module.

### Phase 2 — build surface

1. Code defines → `WARPLOOM_*` primaries; CMake defines BOTH
   (`OMNICPP_X` alias retained for one window). `OMNICPP_CONTRACT` →
   `WARPLOOM_CONTRACT` with a compat `#define` in contract.hpp.
2. CMake options → `WARPLOOM_*` with deprecated-override fallback
   (`if(DEFINED OMNICPP_X AND NOT DEFINED WARPLOOM_X)`); presets
   renamed; the render module's superproject-switch fallback flips to
   the new name (old name still honored).
3. Env vars: code checks `WARPLOOM_*` first, falls back to `OMNICPP_*`
   (real runtime shim).
4. Binaries → `warploom_*` OUTPUT_NAMEs + renamed add_tests; CI
   workflow + docs updated; one-window post-build copies keep old names
   on disk.
5. Root project → `Warploom`; old `OmniCppTemplate` package config dir
   replaced by a stub that includes the new one.
6. Footer guards → `WARPLOOM_COMPAT_*_NS` (header-internal scaffolding;
   not consumer surface).

### Phase 3 — ABI + strings + repo rename

1. Script-module loader accepts BOTH symbol families: tries
   `warploom_module_*` first, falls back to `omnicpp_module_*`; new
   fixtures export the new family; old-family modules keep loading
   (the shim). Contract string → `"warploom contract violation"`
   (test updated; the old string was never parsed by anything).
2. GitHub repo rename `OmniCPP-template` → `warploom`: **manual step**
   (owner action in GitHub UI) — local prep: remote URL update,
   badge/docs sweep, CI trigger paths. Last, after everything else
   passes with the new identity.

## 3. Risks

1. **Editor merge name capture** — after the merge, unqualified names
   in the two `warploom::editor` families could collide (core session
   vs widgets). Census: no identifier collisions found (distinct type
   names; the two families already cross-reference cleanly —
   ClipTimelineView consumes session types rooted today).
2. **Directive-injection hides real decls** (S2-B lesson #4): no real
   `namespace omnicpp::editor/render/asset` member declarations exist
   outside the modules being re-badged (grep-verified census §1.1).
3. **`using namespace` ambiguity in root-suite TUs**: consumers include
   core + render/editor/asset headers simultaneously; the compat
   directives nominate DISTINCT target namespaces — same shape as
   today (core+ui already coexist).
4. **CI/proof scripts referencing old names**: live_proof.py spawns
   hosts by path (updated in phase 2 with the binary renames); CI
   workflow likewise — both are repo files, in scope.

## 4. Verification (per phase, machine-checked)

Four-leg matrix × 6 suites; fresh install; ALL consumer proofs
(CORE/UI/EDITOR/RENDER/ASSET/ENGINE — they spell `omnicpp::*` and
therefore exercise the compat path by design); 64/64 live proofs;
`check_docs_links.py`. Phase 2 additionally: configure with ONLY old
names (`-DOMNICPP_USE_VULKAN=ON`) and ONLY new names — both must work.

## 5. Non-goals

- No new features; no header path changes (`engine/*` forwarders stay).
- No removal of the `omnicpp_*` C ABI family in this pass (dual-export
  is the steady state; dropping old-family loading is post-0.1).
- No GitHub-side actions (manual, §2 phase 3).
