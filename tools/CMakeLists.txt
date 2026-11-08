cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Headless proof host — EditorSession + ControlServer without GPU/window,
# so tools/live_proof.py runs identically in CI and on hardware.
# ============================================================================

add_executable(warploom_headless_host headless_host.cpp)
set_target_properties(warploom_headless_host PROPERTIES OUTPUT_NAME warploom_headless_host)  # S5-B phase 2 binary rename
target_link_libraries(warploom_headless_host PRIVATE Warploom::engine)
target_compile_features(warploom_headless_host PRIVATE cxx_std_26)
