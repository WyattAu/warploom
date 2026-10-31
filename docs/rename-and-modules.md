| Escapement | Rejected; taken on all three registries; crates.io `escapement` is a deterministic virtual clock crate (direct conceptual collision, Jul 2026). |
| Axisflow | Rejected; GitHub org taken; AxisFlow FreeCAD add-on is active (2026 releases). Best of the original five; second choice. |
| Weftline, Loomwright, Loomforge, Warpforge, Heddle, Tactus, Selvage, Warpwright, Jacquard | Rejected; each has active software users in search. |
| Verdant, Loomline, Tessera, Kestrel (original shortlist) | Rejected; all taken; Tessera and Kestrel heavily (UI framework / ASP.NET Core server). |
   - content: `core/` minus editor pieces; Result, contracts, clocks,
   - content: `ui/` + `editor/glyphs`; widget tree, layout, paint list,
   - content: `render/`; Vulkan context/swapchain/renderer, scene
   - content: `editor/`; session, control server/protocol, node editor
  ; **DONE**: lives in `modules/ui` as package `WarploomUI`, exported
3. Then core; **phase A DONE**: lives in `modules/core` as package
   identifier migration, wired to the S-decision pass; the
- No header shuffling beyond what the seams already dictate; the split