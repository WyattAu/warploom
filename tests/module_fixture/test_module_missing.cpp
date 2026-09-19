//! @file test_module_missing.cpp
//! @brief Malformed fixture: exports abi + name but NOT omnicpp_module_tick —
//!        the host must reject it with a precise "missing symbol" error.

#include <cstdint>

extern "C" std::int32_t omnicpp_module_abi() { return 1; }

extern "C" const char* omnicpp_module_name() { return "fixture_missing"; }
