# Pong Game Example CMakeLists.txt

cmake_minimum_required(VERSION 3.20)
project(omnicpp_pong_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Find Vulkan (optional, for rendering)
find_package(Vulkan QUIET)

# Find Quill (logging library)
find_package(quill REQUIRED)

# Find GLM (math library)
find_package(glm QUIET)

# Include directories
set(ENGINE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

# Pong game source files
set(PONG_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/pong_game.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

set(PONG_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/pong_game.hpp
)

# Create pong executable
add_executable(omnicpp_pong_example
    ${PONG_SOURCES}
    ${PONG_HEADERS}
)

# Include directories
target_include_directories(omnicpp_pong_example PRIVATE
    ${ENGINE_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link Qt6 and engine
target_link_libraries(omnicpp_pong_example PRIVATE
    omnicpp_engine
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Qt6 Vulkan integration
target_compile_definitions(omnicpp_pong_example PRIVATE WARPLOOM_HAS_QT_VULKAN)

# Link Vulkan if available
if(Vulkan_FOUND)
    target_include_directories(omnicpp_pong_example PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(omnicpp_pong_example PRIVATE ${VULKAN_LIBRARIES})
    target_compile_definitions(omnicpp_pong_example PRIVATE WARPLOOM_HAS_VULKAN)
    message(STATUS "Vulkan support enabled for pong example")
endif()

# Link Quill
target_link_libraries(omnicpp_pong_example PRIVATE quill::quill)

# Link GLM if available
if(glm_FOUND)
    target_link_libraries(omnicpp_pong_example PRIVATE glm::glm)
    message(STATUS "GLM support enabled for pong example")
endif()

# Wayland-specific settings
if(WARPLOOM_HAS_WAYLAND)
    target_compile_definitions(omnicpp_pong_example PRIVATE WARPLOOM_HAS_WAYLAND)
    message(STATUS "Wayland support enabled for pong example")
endif()

# Set C++ standard
target_compile_features(omnicpp_pong_example PRIVATE cxx_std_23)

# Enable Qt6 MOC, UIC, and RCC
set_target_properties(omnicpp_pong_example PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# Installation
install(TARGETS omnicpp_pong_example
    RUNTIME DESTINATION bin
)
