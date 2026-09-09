cmake_minimum_required(VERSION 3.20)

# ============================================================================
# OmniCpp Viewport — windowed PBR scene application
# ============================================================================

add_executable(omnicpp_viewport main.cpp)
target_include_directories(omnicpp_viewport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(omnicpp_viewport PRIVATE omnicpp_runtime)
target_compile_features(omnicpp_viewport PRIVATE cxx_std_23)
