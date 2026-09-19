//! @file test_module_ok.cpp
//! @brief Well-formed script-module fixture (real .so for the dlopen path).
//! Contract: omnicpp_module_abi / omnicpp_module_name / omnicpp_module_tick.

#include <cstdint>

extern "C" std::int32_t omnicpp_module_abi() { return 1; }

extern "C" const char* omnicpp_module_name() { return "fixture_ok"; }

//! Pure function of (dt, inputs): outputs[i] = inputs[i] * 2 + dt.
extern "C" std::int32_t omnicpp_module_tick(double dt, const double* inputs,
                                            std::uint32_t input_count,
                                            double* outputs,
                                            std::uint32_t output_capacity) {
  if (outputs == nullptr && output_capacity != 0U) {
    return -1;
  }
  const std::uint32_t n =
      input_count < output_capacity ? input_count : output_capacity;
  for (std::uint32_t i = 0; i < n; ++i) {
    outputs[i] = inputs[i] * 2.0 + dt;
  }
  return static_cast<std::int32_t>(n);
}
