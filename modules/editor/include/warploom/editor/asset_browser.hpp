//! @file asset_browser.hpp
//! @brief G5 asset browser (engine half): sorted scene-file listing.
//!
//! The viewport renders the panel (buttons, layout, clicks); this module
//! owns the only part worth testing: scanning a directory for loadable
//! scene documents (*.json — the format save_document writes) and returning
//! them in byte-deterministic sorted order. No rendering, no session, no
//! window system, so it runs headlessly on every CI leg.
//!
//! Errors (missing dir, unreadable entries) yield an empty list, never an
//! exception: an empty browser panel is the honest display of "nothing
//! loadable here".

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace warploom::editor {

//! Filenames (not paths) of *.json regular files directly inside `dir`,
//! sorted ascending by byte order. Deterministic: readdir order is
//! filesystem-dependent, so the result is always sorted before return.
//! Returns empty (never throws) when the directory cannot be read.
[[nodiscard]] std::vector<std::string> list_scene_files(const std::string& dir);

//! True when `name` is a candidate scene file (*.json suffix, more than
//! just the suffix). Exposed for tests.
[[nodiscard]] bool is_scene_file(const std::string& name) noexcept;

//! Scene-panel status line (D3): "scene: <path> *" when dirty, plain
//! when clean; "(untitled)" when there is no current path. Pure, so the
//! panel text is pinnable headlessly like the scan.
[[nodiscard]] std::string scene_status_text(const std::string& current_path,
                                            bool doc_dirty);

}  // namespace warploom::editor
