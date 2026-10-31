# warploom-editor: S4 dependency analysis and migration plan
  `inspector.cpp` 216). Total extraction surface: **~1.5k LOC**; an order
  layer only through `omnicpp::editor::EditorSession` (core; no widget
`omnicpp::render`); no render header needs to move for this split. The
- `node_editor`; `NodeEditorView` (pure projection from `NodeGraph` to
- `inspector`; `InspectorPanel` (outliner/properties/bindings
- `graph_anim_bridge`; `GraphSignalAdapter` (graph outputs →
- `TimelinePanel` (main.cpp:186–325, ~140 lines); tempting because it is
- `ViewportControlHost` (main.cpp:726–914); the app's ControlHost
- `setup_node_editor` / `tick_node_editor` (main.cpp:4450–4679); the
scope for main.cpp is deliberately narrow; delete what the module makes
| Click routing (toolbar/inspector/node) | ~1150–1280 | Keep; thin, already module-shaped |
| `setup_node_editor` | 4450 | Keep; demo seeding is app content |
| `tick_node_editor` | 4568 | Keep; glue, shrinks only if module API absorbs it |
of main.cpp additions; the roadmap's stated motivation for extracting
## Strategy: phase A of S4: the package, zero identifier churn
     in the template (core is pulled transitively via ui? NO; ui does not
     OWN include roots, not the superproject's forwarders; forwarders
   tests; rasterization is a view concern and the test exercises the
     REQUIRED)` consumer binary runs (`EDITOR_CONSUMER_OK`); build a
     unchanged, but the runtime link graph changed; prove it).
   the headers verbatim; no churn until phase B.
   the rasterizer (they don't; see allowlist note in CMake comments).
   `omnicpp_runtime` only (session, no widgets); it must keep working with
   first two-deep module; the consumer proof is the check.