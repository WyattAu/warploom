# Loop 18 — asset streaming and compression (research loop 8 of 10)

Status: notes + fix, 2026-10-26.

## Sources

- **Khronos KTX 2.0 specification (KTX-Software, khronos.org)** (vendor
  standard, labelled as such): 80-byte header, level index of
  (byteOffset, byteLength, uncompressedByteLength) triples, DFD/KVD
  sections, `supercompressionScheme` (0 = none, 1 = BasisLZ, 2 = Zstd,
  3 = Zlib). Our decoder accepts scheme 0 + RGBA8 only and rejects the
  rest loudly — the spec tells us exactly which field values mean
  "transcode me", so rejection is principled, not a gap we stumbled on.
- **Basis Universal (Binomial / Khronos)** (vendor documentation,
  labelled as such): supercompressed payloads need a transcoder that
  outputs GPU block formats per-device. Adopting it would trade our
  transport-independent `DecodedImage` (same bytes on every build, feeds
  identical upload paths) for device-dependent bytes — a determinism
  cost this engine's replay contract cannot pay today. Rejection stays.
- **glTF 2.0 specification (Khronos)** (vendor standard): bufferViews +
  accessors describe strided binary blobs; the format has NO streaming
  primitive — ingestion is whole-buffer by design. Our importer matches:
  it validates the whole document before touching payloads.
- **Wimmer et al., virtual-texture feedback (idTech 6 mega-texture
  lineage)** (practitioner canon, NOT peer-reviewed — labelled as such):
  the render pass reports which tiles are needed, the streamer pages them
  in. The closest shipped analogue to a "browser reflecting runtime
  state" (README follow-up). Related, NOT adopted: we have no virtual
  texture, no tile residency, and no runtime page demand — adopting the
  feedback half without the residency half would be a counter that
  drives nothing.
- **Our code**: `decode_png` / `decode_jpeg` caps (`kMaxDecodedPngBytes`,
  `kMaxDecodedJpegBytes`, both 256 MiB), `decode_ktx2` (no cap),
  `DecodedImage::valid()` (32-bit product).

## Claims being borrowed

1. A whole-file image decoder must bound its allocation before touching
   payloads (PNG/JPEG precedent in-tree). The bound is 256 MiB RGBA.
2. Supercompressed/foreign-format payloads are rejected with a named
   diagnostic, never misinterpreted (KTX2 spec field semantics).
3. Feedback-driven streaming requires BOTH a demand signal and tile
   residency; a demand counter alone is not streaming.

## Novelty assessment

None. The fix extends the in-tree cap discipline to the one decoder
missing it and repairs a 32-bit wrap in the shared validator.

## What fits Warploom, and what it costs

**The defect: KTX2 had no decoded-size cap.** PNG and JPEG both reject
dimensions whose `w*h*4` exceeds 256 MiB before allocating; KTX2 walked
the level index and then `resize`d `width*height*4` unchecked — a hostile
header claiming 30000x30000 attempts a 3.6 GiB vector before any bounds
check (bad_alloc / OOM from untrusted bytes). Fix: `kMaxDecodedKtx2Bytes`
(same 256 MiB), checked as a 64-bit product immediately after the
nonzero test, before the level-index walk and the `resize`. Level 0 is
the largest mip and the only one returned, so capping it caps the
allocation. Cost: textures above 256 MiB decoded (e.g. 16384x16384 RGBA)
are rejected like their PNG/JPEG equivalents — consistent, not new.

**The companion hole, same contract: `valid()` wrapped.** It compared
`rgba.size()` against `static_cast<size_t>(width)*height*4U` — 32-bit
arithmetic on typical builds, so 65536x65536 aliases a small size and a
truncated buffer validates. Fix: 64-bit product. No caller changes; all
existing valid images still validate (products below 2^32 are identical).

**Audited, no change:** supercompression rejection (scheme + named
non-RGBA formats), the per-level spec-size validation, `in_bounds`
offset+size arithmetic (already `length - offset` ordered), and the
magic-byte dispatch. Streaming (paged/mipped upload, background decode)
is recorded as future work with its prerequisite: tile residency must
land first, or the feedback counter drives nothing.

## What was measured

- New tests: `RejectsDimensionsAboveDecodedSizeCap` (hostile 30000x30000
  header on an otherwise-valid container names the cap — proving the
  check fires before payload handling), `ValidRejectsWrapped32BitProduct`.
- Sabotage A/B by real rebuild: cap check removed → first test FAILS
  (decode attempts the 3.6 GiB resize path instead of rejecting);
  `valid()` reverted to 32-bit → second test FAILS. Fixed file passes.
- Asset decoder suites below (png + jpeg + ktx2 + gltf importer).
- Lean `proofs/AssetBounds.lean`: mip-size formula monotonicity
  (level+1 ≤ level, so level 0 dominates), cap-check soundness (accepted
  ⇒ allocation ≤ 256 MiB), no axioms.
