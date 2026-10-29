# Loop 24 — in-tree BC1/BC3 decode for KTX2 (first of the content-pipeline arc)

## Evaluate (written before starting)

glTF import is DONE (mesh + scene + skeletal, mannequin + Sponza, GPU
end-to-end tests) — "nothing loads real content" from the Loop 23 note is
wrong about geometry and right about one thing: `rendering-status.md` says
"Compressed textures beyond RGBA8: no — the KTX2 decoder accepts
uncompressed RGBA8 only". Any real-world KTX2 with a BC payload fails the
load. That is a silent constraint on content, so removing it is the step.

## Sources

Format implementation, not research — the sources are the specs, all read
as the normative text for the exact formulas implemented:

- Khronos KTX File Format Specification 2.0
  (registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html) — vendor standard.
  The claims borrowed: header/level-index layout (§3), mip dimensions
  (`max(1, w >> level)`-style per-level sizing used by `level_bytes`),
  `uncompressedByteLength` semantics, `supercompressionScheme == 0` means
  the payload is stored, not transcoded. Already cited in Loop 18; the
  Loop 18 re-read for streaming stands.
- Microsoft "Block Compression (BC)" reference
  (learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression)
  — vendor documentation, not evidence. The claims borrowed: the BC1
  4-color/3-color+transparent endpoint rules, the BC3 8-alpha/6-alpha
  endpoint rules, and the `(2c0+c1)/3`-family interpolation weights.
- OpenGL `EXT_texture_compression_s3tc` (registry.khronos.org/OpenGL/
  extensions/EXT/EXT_texture_compression_s3tc.txt) — vendor documentation.
  Same math from the other API; used as the cross-check that the
  interpolation directions agree.
- Vulkan format values measured, not assumed: 131 (`BC1_RGB_UNORM_BLOCK`)
  through 138 from `/usr/include/vulkan/vulkan_core.h` on this machine.
- S3TC patents (US 6,775,392 et al.) expired 2017–2018: public context for
  why shipping an in-tree decoder is unencumbered. Stated as context, not
  as legal advice.

## The claim being borrowed

A BC1 block is two 565 endpoints + 16 2-bit interpolants; a BC3 block is
two alpha endpoints + 16 3-bit alpha interpolants + a BC1 color block.
The interpolation weights above decode every texel in integer math — no
float, no tables, no LUT divergence across builds.

## Novelty assessment

None. BC decoders are decades old (stb_dxt, squish, every GPU). The fit is
the combination, and it is the estate's combination: PNG/JPEG already
decode in-tree (decoders are a determinism feature here, S3.5), so BC1/BC3
join them rather than arriving via a transcoder dependency. Loop 18
rejected Basis Universal to protect transport-independent bytes; this is
the other half of that decision — accept the formats a transcoder would
have produced, decoded deterministically, with supercompression still
refused.

## Why it fits here, and what it costs

- Fits: `DecodedImage` is already "tightly packed RGBA8, consumers never
  care which codec produced the bytes" — the decoder plugs into the
  contract, and the PNG-proven upload path carries it to the GPU
  unchanged. KTX2 SRGB variants decode identically; sRGB-ness stays an
  upload interpretation, exactly as for RGBA8 37-vs-43 today.
- Costs: ~200 lines, zero dependencies, integer-only math. The conscious
  trade: decoded RGBA8 costs 4–8x the VRAM of a natively uploaded BC
  texture. GPU-native compressed upload (format support queries,
  compressed copy paths) is a second model and stays deferred — recorded
  with the trigger (measured VRAM pressure on a real scene) rather than
  built on speculation.
- Deferred with triggers: BC2 (no real asset in tree needs it; add when
  one does), BC4/5/6H/7, ETC2/ASTC (same), supercompressed payloads
  (BasisLZ/UASTC — Loop 18's rejection stands until a transcoder-shaped
  need is measured, not assumed).

## What was measured

- `test_bc_decode.cpp`: hand-computed vectors (pure-red/pure-blue
  endpoints — every interpolated value computed by hand from the spec
  weights, not copied from a run), both BC1 endpoint modes, both BC3
  alpha modes, the BC3 transparent corner, multi-block grid with
  non-multiple-of-4 crop, sabotage A/B, all green (8/8).
- Sabotage A/B, both precise: `/3 → /2` in the endpoint mixer failed
  exactly `Bc1OpaquePaletteIsExact` + `Bc3EightAlphaModeIsExact` (the
  BC3 color block shares the mixer; the all-code-0 and halved-mix tests
  correctly did not fire). `/7 → /8` in the alpha interpolant failed
  exactly `Bc3EightAlphaModeIsExact` (all-index-0 tests correctly did
  not fire — the divisor is never touched on that path). A first
  sabotage attempt was a no-op (sed pattern did not match the
  `mix_endpoint` helper the code actually uses) and passed — caught by
  diffing the file before concluding anything, per the standing rule.
- Two test-byte bugs of mine, both in hand-packed LSB-first alpha
  fields (byte1 0xCC→0xC6; the all-code-5 pack is a 3-byte unit
  0x6D/0xDB/0xB6, not a 1-byte repeat — period 3 against period 8).
  The decoder was right both times; the fixtures were fixed, not the
  code. Hand-computed vectors cut both ways: they catch real bugs,
  but a wrong hand computation looks exactly like a code bug until
  the packing is re-derived bit by bit.
- `test_ktx2_decoder.cpp`: BC1/BC3 containers decode level 0 exactly;
  hostile variants (truncated block, wrong block size, bad typeSize,
  ETC2 still rejected naming the format, supercompression still refused).
- GPU equivalence is stated, not re-proven: decoded bytes enter the same
  `DecodedImage → upload` path the PNG end-to-end test executes. A
  duplicate GPU test would exercise the uploader, not the decoder.
- Baseline at the loop boundary (asset-only change, so the Vulkan/
  proof half is unaffected and not re-run): default preset full build
  warning-free, ctest 6/6, full suite 658/658; asan-ubsan suite 581
  passed / 0 failed / 0 ASan+UBSan diagnostics (39 skips are the
  Vulkan-gated tests, expected with `WARPLOOM_USE_VULKAN=OFF`).
