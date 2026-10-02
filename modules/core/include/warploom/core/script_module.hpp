#pragma once

//! @file script_module.hpp
//! @brief Native scripting module host: C ABI for C++ and Rust gameplay
//!        modules (the Rust path via `#[no_mangle] pub extern "C"`).
//!
//! Why C ABI (not a scripting VM): the project direction is that capable
//! developers write real C++ or Rust — no interpreted scripting layer. The
//! C ABI is the one interface both languages expose with zero runtime
//! dependency, and it keeps modules hot-swappable at the SO boundary.
//!
//! Module contract (either language):
//!   EXPORT int32_t omnicpp_module_abi(void);          // returns 1
//!   EXPORT const char* omnicpp_module_name(void);     // stable name
//!   EXPORT int32_t omnicpp_module_tick(double dt,     // fixed timestep
//!                                      const double* inputs,
//!                                      uint32_t input_count,
//!                                      double* outputs,
//!                                      uint32_t output_capacity);
//! `tick` returns the number of outputs written (<= capacity) or a negative
//! error code. Everything is plain data: no exceptions, no allocation
//! across the boundary, deterministic by construction (fixed dt in, values
//! out) — the same replay contract as the rest of the engine.
//!
//! Headless testing: `ScriptModule` can also load a "builtin" module
//! registered in-process (no dlopen), so the full call path is testable on
//! every CI leg without building a shared object.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace warploom::core {

//! ABI version this host speaks.
inline constexpr std::int32_t kScriptModuleAbi = 1;

struct ScriptModuleApi final {
  std::int32_t (*abi_version)() noexcept = nullptr;
  const char* (*name)() noexcept = nullptr;
  std::int32_t (*tick)(double dt, const double* inputs, std::uint32_t input_count,
                       double* outputs, std::uint32_t output_capacity) noexcept =
      nullptr;
};

//! Host-side handle to one loaded module (dlopen'd or in-process builtin).
class ScriptModule final {
 public:
  ~ScriptModule();
  ScriptModule(ScriptModule&&) noexcept;
  ScriptModule& operator=(ScriptModule&&) noexcept;
  ScriptModule(const ScriptModule&) = delete;
  ScriptModule& operator=(const ScriptModule&) = delete;

  //! Loads a shared object by path and resolves the module contract.
  //! Returns nullptr + `error` on failure (missing file, missing symbols,
  //! ABI mismatch).
  [[nodiscard]] static std::unique_ptr<ScriptModule> load_shared(
      const std::string& path, std::string& error);

  //! Registers a builtin (in-process) module — headless testing and engine
  //! built-ins. Name must be unique; returns false on duplicate.
  [[nodiscard]] static bool register_builtin(std::string name,
                                             ScriptModuleApi api);
  //! Loads a builtin by name. Returns nullptr + `error` when unknown.
  [[nodiscard]] static std::unique_ptr<ScriptModule> load_builtin(
      std::string_view name, std::string& error);

  [[nodiscard]] std::string_view module_name() const noexcept;
  //! Calls the module tick. Returns the output count, or -1 on module error.
  [[nodiscard]] std::int32_t tick(double dt, const double* inputs,
                                  std::uint32_t input_count, double* outputs,
                                  std::uint32_t output_capacity) const noexcept;

 private:
  ScriptModule() = default;
  ScriptModuleApi api_{};
  void* handle_{nullptr};  // dlopen handle (shared-object modules)
  bool is_builtin_{false};
};

//! Determinism gate shared with the replay system: module tick must be a
//! pure function of (dt, inputs). The host cannot enforce purity, but the
//! builtin test modules are proven deterministic byte-for-byte in tests.

}  // namespace warploom::core
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).
#ifndef WARPLOOM_COMPAT_CORE_NS
#define WARPLOOM_COMPAT_CORE_NS
namespace omnicpp::core {
    using namespace ::warploom::core;
}
#endif  // WARPLOOM_COMPAT_CORE_NS
