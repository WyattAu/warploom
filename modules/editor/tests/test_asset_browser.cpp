//! @file test_asset_browser.cpp
//! @brief G5 asset browser proofs: suffix gating, sorted determinism,
//!        and empty-on-unreadable (never throws, never hides real files).

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "warploom/editor/asset_browser.hpp"

namespace {

namespace fs = std::filesystem;
namespace ed = warploom::editor;

TEST(AssetBrowser, SuffixGating) {
  EXPECT_TRUE(ed::is_scene_file("scene.json"));
  EXPECT_TRUE(ed::is_scene_file("a.b.json"));
  EXPECT_FALSE(ed::is_scene_file(".json"));  // suffix alone is not a file
  EXPECT_FALSE(ed::is_scene_file("scene.JSON"));
  EXPECT_FALSE(ed::is_scene_file("scene.jsonl"));
  EXPECT_FALSE(ed::is_scene_file("json"));
  EXPECT_FALSE(ed::is_scene_file(""));
  EXPECT_FALSE(ed::is_scene_file("scene.gltf"));
}

TEST(AssetBrowser, ListsSortedJsonOnly) {
  const fs::path dir =
      fs::temp_directory_path() /
      ("warploom_asset_browser_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  for (const char* name : {"zebra.json", "apple.json", "mango.json",
                           "notes.txt", "model.gltf", ".json"}) {
    std::ofstream(dir / name) << "{}\n";
  }
  fs::create_directories(dir / "nested.json");  // a dir, not a file

  const auto files = ed::list_scene_files(dir.string());
  // Sorted ascending, JSON files only (notes/model/hidden-suffix excluded;
  // the nested.json directory is not a load button).
  ASSERT_EQ(files.size(), 3U) << "dir=" << dir.string();
  EXPECT_EQ(files[0], "apple.json");
  EXPECT_EQ(files[1], "mango.json");
  EXPECT_EQ(files[2], "zebra.json");
  // Deterministic across calls (readdir order is filesystem-dependent).
  EXPECT_EQ(ed::list_scene_files(dir.string()), files);
  fs::remove_all(dir);
}

TEST(AssetBrowser, SceneStatusText) {
  // D3 dirty indicator: path + star state is the whole contract.
  EXPECT_EQ(ed::scene_status_text("", false), "scene: (untitled)");
  EXPECT_EQ(ed::scene_status_text("", true), "scene: (untitled) *");
  EXPECT_EQ(ed::scene_status_text("/tmp/demo.json", false),
            "scene: /tmp/demo.json");
  EXPECT_EQ(ed::scene_status_text("/tmp/demo.json", true),
            "scene: /tmp/demo.json *");
}

TEST(AssetBrowser, MissingDirIsEmptyNotAnException) {
  EXPECT_TRUE(ed::list_scene_files("/no/such/warploom/dir").empty());
  EXPECT_TRUE(ed::is_scene_file("x.json"));  // gating still works
}

}  // namespace
