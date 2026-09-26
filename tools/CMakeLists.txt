cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Headless proof host — EditorSession + ControlServer without GPU/window,
# so tools/live_proof.py runs identically in CI and on hardware.
# ============================================================================

add_executable(omnicpp_headless_host headless_host.cpp)
target_include_directories(omnicpp_headless_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(omnicpp_headless_host PRIVATE omnicpp_runtime)
target_compile_features(omnicpp_headless_host PRIVATE cxx_std_26)
