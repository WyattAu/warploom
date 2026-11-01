//! @file script_module.cpp
//! @brief Script-module host bodies (see the header).

#include "warploom/core/script_module.hpp"

#include <dlfcn.h>

#include <map>
#include <mutex>
#include <utility>

#include "warploom/core/contract.hpp"

namespace warploom::core {

namespace {

std::mutex& builtin_mutex() {
  static std::mutex m;
  return m;
}

std::map<std::string, ScriptModuleApi>& builtin_registry() {
  static std::map<std::string, ScriptModuleApi> registry;
  return registry;
}

}  // namespace

ScriptModule::~ScriptModule() {
  if (handle_ != nullptr) {
    dlclose(handle_);
  }
}

ScriptModule::ScriptModule(ScriptModule&& other) noexcept
    : api_(other.api_), handle_(other.handle_), is_builtin_(other.is_builtin_) {
  other.handle_ = nullptr;
}

ScriptModule& ScriptModule::operator=(ScriptModule&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
    api_ = other.api_;
    handle_ = other.handle_;
    is_builtin_ = other.is_builtin_;
    other.handle_ = nullptr;
  }
  return *this;
}

std::unique_ptr<ScriptModule> ScriptModule::load_shared(
    const std::string& path, std::string& error) {
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    error = "dlopen failed: " + std::string(dlerror());
    return nullptr;
  }
  // S5-B phase 3 (docs/warploom-identity-plan.md): the warploom_module_*
  // C ABI family is primary; legacy omnicpp_module_* modules keep loading
  // through the per-symbol fallback (removed post-0.1).
  const auto resolve = [&](const char* suffix) {
    void* ptr = dlsym(handle, (std::string("warploom_module_") + suffix).c_str());
    if (ptr != nullptr) return ptr;
    ptr = dlsym(handle, (std::string("omnicpp_module_") + suffix).c_str());
    if (ptr == nullptr) {
      error = std::string("missing symbol warploom_module_") + suffix +
              " (and legacy omnicpp_module_" + suffix + ") in " + path;
    }
    return ptr;
  };
  ScriptModuleApi api;
  *reinterpret_cast<void**>(&api.abi_version) = resolve("abi");
  if (!error.empty()) {
    dlclose(handle);
    return nullptr;
  }
  *reinterpret_cast<void**>(&api.name) = resolve("name");
  if (!error.empty()) {
    dlclose(handle);
    return nullptr;
  }
  *reinterpret_cast<void**>(&api.tick) = resolve("tick");
  if (!error.empty()) {
    dlclose(handle);
    return nullptr;
  }
  if (api.abi_version() != kScriptModuleAbi) {
    error = "module ABI " + std::to_string(api.abi_version()) +
            " != host ABI " + std::to_string(kScriptModuleAbi);
    dlclose(handle);
    return nullptr;
  }
  auto module = std::unique_ptr<ScriptModule>(new ScriptModule());
  module->api_ = api;
  module->handle_ = handle;
  module->is_builtin_ = false;
  return module;
}

bool ScriptModule::register_builtin(std::string name, ScriptModuleApi api) {
  OMNICPP_CONTRACT(api.abi_version != nullptr);
  OMNICPP_CONTRACT(api.name != nullptr);
  OMNICPP_CONTRACT(api.tick != nullptr);
  const std::lock_guard<std::mutex> lock(builtin_mutex());
  return builtin_registry().emplace(std::move(name), api).second;
}

std::unique_ptr<ScriptModule> ScriptModule::load_builtin(
    std::string_view name, std::string& error) {
  const std::lock_guard<std::mutex> lock(builtin_mutex());
  const auto it = builtin_registry().find(std::string(name));
  if (it == builtin_registry().end()) {
    error = "no builtin module named \"" + std::string(name) + "\"";
    return nullptr;
  }
  if (it->second.abi_version() != kScriptModuleAbi) {
    error = "builtin ABI mismatch";
    return nullptr;
  }
  auto module = std::unique_ptr<ScriptModule>(new ScriptModule());
  module->api_ = it->second;
  module->handle_ = nullptr;
  module->is_builtin_ = true;
  return module;
}

bool ScriptModule::parse_module_spec(std::string_view entry,
                                      std::string& path,
                                      std::uint32_t& inputs,
                                      std::uint32_t& outputs) {
  inputs = 1U;
  outputs = 1U;
  if (entry.empty()) {
    return false;
  }
  std::string_view rest = entry;
  const std::size_t c1 = rest.find(':');
  std::string_view count_part;
  if (c1 != std::string_view::npos) {
    path = std::string(rest.substr(0U, c1));
    count_part = rest.substr(c1 + 1U);
  } else {
    path = std::string(rest);
    return !path.empty();
  }
  if (path.empty()) {
    return false;
  }
  const auto parse_count = [](std::string_view s, std::uint32_t& v) {
    if (s.empty()) {
      return false;
    }
    for (const char c : s) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    std::uint64_t wide = 0;
    for (const char c : s) {
      wide = wide * 10U + static_cast<std::uint64_t>(c - '0');
      if (wide > 9U) {
        return false;
      }
    }
    v = static_cast<std::uint32_t>(wide);
    return v >= 1U;
  };
  const std::size_t c2 = count_part.find(':');
  if (c2 == std::string_view::npos) {
    return parse_count(count_part, inputs);
  }
  return parse_count(count_part.substr(0U, c2), inputs) &&
         parse_count(count_part.substr(c2 + 1U), outputs);
}

std::string_view ScriptModule::module_name() const noexcept {
  return api_.name != nullptr ? std::string_view(api_.name())
                              : std::string_view{};
}

std::int32_t ScriptModule::tick(double dt, const double* inputs,
                                std::uint32_t input_count, double* outputs,
                                std::uint32_t output_capacity) const noexcept {
  return api_.tick != nullptr
             ? api_.tick(dt, inputs, input_count, outputs, output_capacity)
             : -1;
}

}  // namespace warploom::core
