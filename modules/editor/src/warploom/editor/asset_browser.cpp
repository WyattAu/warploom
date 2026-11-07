//! @file asset_browser.cpp
//! @brief G5 asset browser scan (see the header).

#include "warploom/editor/asset_browser.hpp"

#include <dirent.h>

#include <algorithm>

namespace warploom::editor {

bool is_scene_file(const std::string& name) noexcept {
  constexpr const char* kSuffix = ".json";
  constexpr std::size_t kLen = 5;
  return name.size() > kLen &&
         name.compare(name.size() - kLen, kLen, kSuffix) == 0;
}

std::vector<std::string> list_scene_files(const std::string& dir) {
  std::vector<std::string> out;
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) {
    return out;  // missing/unreadable dir: empty panel, no exception
  }
  while (dirent* entry = ::readdir(handle)) {
    // d_name is a char array, never null; the comparison read as a guard
    // but the compiler correctly flags it as tautological.
    const std::string name = entry->d_name;
    if (!is_scene_file(name)) {
      continue;
    }
    // Regular files only: skip directories/symlinks that happen to end
    // in .json (a stale-scan entry must never become a load button).
    // d_type is best-effort (DT_UNKNOWN on some filesystems): fall back
    // to accepting the entry rather than hiding real files.
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_REG &&
        entry->d_type != DT_LNK) {
      continue;
    }
    out.push_back(name);
  }
  ::closedir(handle);
  std::sort(out.begin(), out.end());
  return out;
}

std::string scene_status_text(const std::string& current_path, bool doc_dirty) {
  std::string out = "scene: ";
  out += current_path.empty() ? "(untitled)" : current_path;
  if (doc_dirty) {
    out += " *";
  }
  return out;
}

}  // namespace warploom::editor
