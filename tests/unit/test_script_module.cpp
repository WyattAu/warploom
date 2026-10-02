//! @file test_script_module.cpp
//! @brief Native script-module host proofs.
//!
//! Two load paths, both fully proven:
//!   1. Builtin (in-process) — the full contract path runs on every CI leg,
//!      including the headless ones: registration, load, tick, determinism,
//!      duplicate-name rejection.
//!   2. Shared object (dlopen) — real .so fixtures built by CMake: a
//!      well-formed module, one missing a required symbol, one with a
//!      wrong ABI version. Each malformed case is rejected with a precise
//!      diagnostic.

#include <gtest/gtest.h>

#include <cstring>
#include <cstdint>
#include <string>
#include <utility>

#include "warploom/core/script_module.hpp"

namespace {

using omnicpp::core::ScriptModule;
using omnicpp::core::ScriptModuleApi;

//! Deterministic builtin tick: outputs[i] = inputs[i] + dt * (i + 1).
std::int32_t builtin_tick(double dt, const double* inputs,
                          std::uint32_t input_count, double* outputs,
                          std::uint32_t output_capacity) noexcept {
  if (outputs == nullptr && output_capacity != 0U) {
    return -1;
  }
  const std::uint32_t n =
      input_count < output_capacity ? input_count : output_capacity;
  for (std::uint32_t i = 0; i < n; ++i) {
    outputs[i] = inputs[i] + dt * static_cast<double>(i + 1U);
  }
  return static_cast<std::int32_t>(n);
}

//! Registers the test builtin once per process; returns its API for checks.
std::int32_t builtin_abi() noexcept { return omnicpp::core::kScriptModuleAbi; }
const char* builtin_name() noexcept { return "test_builtin"; }

ScriptModuleApi register_test_builtin() {
  static const ScriptModuleApi api = [] {
    ScriptModuleApi a;
    a.abi_version = &builtin_abi;
    a.name = &builtin_name;
    a.tick = &builtin_tick;
    return a;
  }();
  static const bool registered =
      ScriptModule::register_builtin("test_builtin", api);
  (void)registered;
  return api;
}

}  // namespace

TEST(ScriptModule, BuiltinLoadAndTick) {
  (void)register_test_builtin();
  std::string error;
  auto module = ScriptModule::load_builtin("test_builtin", error);
  ASSERT_NE(module, nullptr) << error;
  EXPECT_EQ(module->module_name(), "test_builtin");

  const double inputs[] = {1.0, 2.0, 3.0};
  double outputs[3] = {};
  const auto n = module->tick(0.5, inputs, 3U, outputs, 3U);
  ASSERT_EQ(n, 3);
  EXPECT_DOUBLE_EQ(outputs[0], 1.0 + 0.5 * 1.0);
  EXPECT_DOUBLE_EQ(outputs[1], 2.0 + 0.5 * 2.0);
  EXPECT_DOUBLE_EQ(outputs[2], 3.0 + 0.5 * 3.0);
}

TEST(ScriptModule, BuiltinTickIsDeterministic) {
  (void)register_test_builtin();
  std::string error;
  auto module = ScriptModule::load_builtin("test_builtin", error);
  ASSERT_NE(module, nullptr) << error;

  const double inputs[] = {0.25, -3.5, 7.0, 1e9};
  double a[4] = {};
  double b[4] = {};
  ASSERT_EQ(module->tick(0.016, inputs, 4U, a, 4U), 4);
  ASSERT_EQ(module->tick(0.016, inputs, 4U, b, 4U), 4);
  EXPECT_EQ(std::memcmp(a, b, sizeof(a)), 0)
      << "same (dt, inputs) must produce byte-identical outputs";
}

TEST(ScriptModule, BuiltinCapacityClampAndNullOutput) {
  (void)register_test_builtin();
  std::string error;
  auto module = ScriptModule::load_builtin("test_builtin", error);
  ASSERT_NE(module, nullptr) << error;

  // More inputs than capacity: tick writes exactly capacity outputs.
  const double inputs[] = {1.0, 2.0, 3.0};
  double outputs[2] = {};
  EXPECT_EQ(module->tick(0.0, inputs, 3U, outputs, 2U), 2);
  EXPECT_DOUBLE_EQ(outputs[0], 1.0);
  EXPECT_DOUBLE_EQ(outputs[1], 2.0);

  // Null output with nonzero capacity is a module error (-1).
  EXPECT_EQ(module->tick(0.0, inputs, 3U, nullptr, 3U), -1);
}

TEST(ScriptModule, BuiltinUnknownNameRejected) {
  std::string error;
  auto module = ScriptModule::load_builtin("no_such_builtin", error);
  EXPECT_EQ(module, nullptr);
  EXPECT_NE(error.find("no builtin module named"), std::string::npos);
}

TEST(ScriptModule, BuiltinDuplicateNameRejected) {
  ScriptModuleApi api = register_test_builtin();
  // Second registration under the same name must fail.
  EXPECT_FALSE(ScriptModule::register_builtin("test_builtin", api));
}

TEST(ScriptModule, SharedObjectWellFormed) {
  std::string error;
  auto module =
      ScriptModule::load_shared(WARPLOOM_TEST_MODULE_OK, error);
  ASSERT_NE(module, nullptr) << error;
  EXPECT_EQ(module->module_name(), "fixture_ok");

  const double inputs[] = {1.0, -2.0};
  double outputs[2] = {};
  ASSERT_EQ(module->tick(0.1, inputs, 2U, outputs, 2U), 2);
  EXPECT_DOUBLE_EQ(outputs[0], 1.0 * 2.0 + 0.1);
  EXPECT_DOUBLE_EQ(outputs[1], -2.0 * 2.0 + 0.1);

  // Deterministic across calls.
  double again[2] = {};
  ASSERT_EQ(module->tick(0.1, inputs, 2U, again, 2U), 2);
  EXPECT_EQ(std::memcmp(outputs, again, sizeof(outputs)), 0);
}

TEST(ScriptModule, SharedObjectMissingFile) {
  std::string error;
  auto module = ScriptModule::load_shared(
      WARPLOOM_TEST_BIN_DIR "/no_such_module.so", error);
  EXPECT_EQ(module, nullptr);
  EXPECT_NE(error.find("dlopen failed"), std::string::npos);
}

TEST(ScriptModule, SharedObjectMissingSymbol) {
  std::string error;
  auto module =
      ScriptModule::load_shared(WARPLOOM_TEST_MODULE_MISSING, error);
  EXPECT_EQ(module, nullptr);
  EXPECT_NE(error.find("missing symbol"), std::string::npos);
  EXPECT_NE(error.find("omnicpp_module_tick"), std::string::npos);
}

TEST(ScriptModule, SharedObjectAbiMismatch) {
  std::string error;
  auto module = ScriptModule::load_shared(WARPLOOM_TEST_MODULE_ABI, error);
  EXPECT_EQ(module, nullptr);
  EXPECT_NE(error.find("ABI"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Rust module path: dlopens the release cdylib of examples/rust_module
// (built by `cargo build --release`, wired into CMake when cargo exists).
// Proves the C ABI contract is language-agnostic: a Rust module loads,
// ticks, and behaves identically to the C++ fixtures.

#if defined(WARPLOOM_TEST_RUST_MODULE)
TEST(ScriptModule, RustModuleLoadsAndTicks) {
  std::string error;
  auto module = ScriptModule::load_shared(WARPLOOM_TEST_RUST_MODULE, error);
  ASSERT_NE(module, nullptr) << error;
  EXPECT_EQ(module->module_name(), "rust_example");

  const double inputs[] = {2.0, 4.0, -6.0};
  double outputs[3] = {};
  ASSERT_EQ(module->tick(0.25, inputs, 3U, outputs, 3U), 3);
  EXPECT_DOUBLE_EQ(outputs[0], 2.0 * 0.5 + 0.25);
  EXPECT_DOUBLE_EQ(outputs[1], 4.0 * 0.5 + 0.25);
  EXPECT_DOUBLE_EQ(outputs[2], -6.0 * 0.5 + 0.25);
}

TEST(ScriptModule, RustModuleDeterministic) {
  std::string error;
  auto module = ScriptModule::load_shared(WARPLOOM_TEST_RUST_MODULE, error);
  ASSERT_NE(module, nullptr) << error;

  const double inputs[] = {0.5, -1.25, 3.75, 1e12};
  double a[4] = {};
  double b[4] = {};
  ASSERT_EQ(module->tick(1.0 / 60.0, inputs, 4U, a, 4U), 4);
  ASSERT_EQ(module->tick(1.0 / 60.0, inputs, 4U, b, 4U), 4);
  EXPECT_EQ(std::memcmp(a, b, sizeof(a)), 0);
}
#endif  // WARPLOOM_TEST_RUST_MODULE
