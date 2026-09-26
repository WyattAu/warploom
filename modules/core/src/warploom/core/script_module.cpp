//! @file script_module.cpp
//! @brief Script-module host bodies (see the header).

#include "warploom/core/script_module.hpp"

#include <dlfcn.h>

#include <map>
#include <mutex>
#include <utility>

#include "warploom/core/contract.hpp"

namespace omnicpp::core {

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
  const auto resolve = [&](const char* symbol) {
    void* ptr = dlsym(handle, symbol);
    if (ptr == nullptr) {
      error = std::string("missing symbol ") + symbol + " in " + path;
    }
    return ptr;
  };
  ScriptModuleApi api;
  *reinterpret_cast<void**>(&api.abi_version) = resolve("omnicpp_module_abi");
  if (!error.empty()) {
    dlclose(handle);
    return nullptr;
  }
  *reinterpret_cast<void**>(&api.name) = resolve("omnicpp_module_name");
  if (!error.empty()) {
    dlclose(handle);
    return nullptr;
  }
  *reinterpret_cast<void**>(&api.tick) = resolve("omnicpp_module_tick");
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

}  // namespace omnicpp::core
