cmake_minimum_required(VERSION 3.20)

# ============================================================================
# OmniCpp Viewport — windowed PBR scene application
# ============================================================================

add_executable(omnicpp_viewport main.cpp)
target_include_directories(omnicpp_viewport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
# S5-A: the aggregate (Warploom::engine) replaces the deleted
# omnicpp_runtime monolith; the include root supplies the engine/*
# forwarder spellings main.cpp and telemetry.hpp use.
target_link_libraries(omnicpp_viewport PRIVATE Warploom::engine)
target_include_directories(omnicpp_viewport PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(omnicpp_viewport PRIVATE cxx_std_26)
