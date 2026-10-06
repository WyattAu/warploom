#pragma once

//! @file module_abi.h
//! @brief C ABI a loadable gameplay module must export.
//!
//! The host resolves all three symbols with dlsym by name (see
//! script_module.cpp), so nothing in the tree declares them and
//! -Wmissing-declarations fires on every module that defines them without a
//! prior declaration. This header is that declaration.
//!
//! Two prefixes exist: `warploom_module_*` is the current name and
//! `omnicpp_module_*` is the legacy one the host still falls back to
//! (script_module.cpp tries warploom first, then omnicpp). Both are declared
//! so a fixture can export either and still compile; a module should export
//! the warploom_ set.
//!
//! It is deliberately a C header with no project types: a module may be built
//! in any language that can export C symbols -- the Rust path uses
//! `#[no_mangle] pub extern "C"` -- so nothing here may require a C++ compiler
//! or a Warploom header. Keep it that way; a member type would silently raise
//! the floor for every future module language.
//!
//! Contract, in the host's order:
//!   - abi_version must equal warploom_module_abi_version() (currently 1), or
//!     the host refuses the module rather than guessing at the layout.
//!   - tick is a pure function of (dt, inputs, count, outputs, capacity).
//!     Fixed dt in and double arrays across the boundary make a module
//!     deterministic by construction, which is why this is a C ABI and not a
//!     scripting VM.
//!   - tick returns the number of outputs written, or negative on error.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! ABI revision this header describes. Must match kScriptModuleAbi.
#define WARPLOOM_MODULE_ABI_VERSION 1

//! Reports the ABI revision the module was built against.
int32_t warploom_module_abi(void);

//! NUL-terminated module name, used in diagnostics and script lookups.
const char* warploom_module_name(void);

//! Pure step. `inputs` holds `input_count` doubles; writes at most
//! `output_capacity` doubles into `outputs`. Returns outputs written, or a
//! negative value on error.
int32_t warploom_module_tick(double dt, const double* inputs,
                             uint32_t input_count, double* outputs,
                             uint32_t output_capacity);

//! Legacy prefix, still resolved as a fallback. Declared so legacy modules
//! compile; do not export these in new modules.
int32_t omnicpp_module_abi(void);
const char* omnicpp_module_name(void);
int32_t omnicpp_module_tick(double dt, const double* inputs,
                            uint32_t input_count, double* outputs,
                            uint32_t output_capacity);

#ifdef __cplusplus
}  // extern "C"
#endif
