//! @file test_module_missing.cpp
//! @brief Malformed fixture (LEGACY family): exports abi + name but NOT the tick symbol —
//!        the host must reject it with a precise "missing symbol" error.

#include <cstdint>

extern "C" std::int32_t omnicpp_module_abi() { return 1; }

extern "C" const char* omnicpp_module_name() { return "fixture_missing"; }
