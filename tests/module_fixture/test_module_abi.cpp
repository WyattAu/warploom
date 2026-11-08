//! @file test_module_abi.cpp
//! @brief Malformed fixture: full contract but declares ABI version 2 — the
//!        host (ABI 1) must reject it with an explicit version error.

#include "module_abi.h"

#include <cstdint>

extern "C" std::int32_t warploom_module_abi() { return 2; }

extern "C" const char* warploom_module_name() { return "fixture_abi2"; }

extern "C" std::int32_t warploom_module_tick(double, const double*,
                                            std::uint32_t, double*,
                                            std::uint32_t) {
  return -2;
}
