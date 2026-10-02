# ============================================================================
# OmniCpp Template - Format Targets
# ============================================================================
# Defines code formatting targets (clang-format, black)
# ============================================================================

if(WARPLOOM_ENABLE_FORMATTING)
    # Find clang-format
    find_program(CLANG_FORMAT_EXECUTABLE clang-format)

    # Find black (Python formatter)
    find_program(BLACK_EXECUTABLE black)

    # C++ formatting
    if(CLANG_FORMAT_EXECUTABLE)
        # Get clang-format style
        set(WARPLOOM_CLANG_FORMAT_STYLE "file" CACHE STRING "Clang-format style")
        set_property(CACHE WARPLOOM_CLANG_FORMAT_STYLE PROPERTY STRINGS "file" "llvm" "google" "chromium" "mozilla" "webkit")

        # Custom target to format C++ files
        add_custom_target(format-cpp
            COMMAND ${CMAKE_COMMAND} -E echo "Formatting C++ files..."
            COMMAND ${CLANG_FORMAT_EXECUTABLE}
            -i
            --style=${WARPLOOM_CLANG_FORMAT_STYLE}
            ${CMAKE_SOURCE_DIR}/include/**/*.hpp
            ${CMAKE_SOURCE_DIR}/include/**/*.h
            ${CMAKE_SOURCE_DIR}/src/**/*.cpp
            ${CMAKE_SOURCE_DIR}/src/**/*.c
            COMMENT "Formatting C++ files with clang-format"
        )

        # Custom target to check C++ formatting
        add_custom_target(format-cpp-check
            COMMAND ${CMAKE_COMMAND} -E echo "Checking C++ formatting..."
            COMMAND ${CLANG_FORMAT_EXECUTABLE}
            --dry-run
            --Werror
            --style=${WARPLOOM_CLANG_FORMAT_STYLE}
            ${CMAKE_SOURCE_DIR}/include/**/*.hpp
            ${CMAKE_SOURCE_DIR}/include/**/*.h
            ${CMAKE_SOURCE_DIR}/src/**/*.cpp
            ${CMAKE_SOURCE_DIR}/src/**/*.c
            COMMENT "Checking C++ formatting with clang-format"
        )

        message(STATUS "clang-format found: ${CLANG_FORMAT_EXECUTABLE}")
    else()
        message(WARNING "clang-format not found, C++ formatting disabled")
    endif()

    # Python formatting
    if(BLACK_EXECUTABLE)
        # Custom target to format Python files
        add_custom_target(format-python
            COMMAND ${CMAKE_COMMAND} -E echo "Formatting Python files..."
            COMMAND ${BLACK_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/omni_scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/tests/**/*.py
            COMMENT "Formatting Python files with black"
        )

        # Custom target to check Python formatting
        add_custom_target(format-python-check
            COMMAND ${CMAKE_COMMAND} -E echo "Checking Python formatting..."
            COMMAND ${BLACK_EXECUTABLE}
            --check
            ${CMAKE_SOURCE_DIR}/scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/omni_scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/tests/**/*.py
            COMMENT "Checking Python formatting with black"
        )

        message(STATUS "black found: ${BLACK_EXECUTABLE}")
    else()
        message(WARNING "black not found, Python formatting disabled")
    endif()

    # Custom target to format all files
    if(CLANG_FORMAT_EXECUTABLE OR BLACK_EXECUTABLE)
        add_custom_target(format
            COMMAND ${CMAKE_COMMAND} -E echo "Formatting all files..."
        )

        if(CLANG_FORMAT_EXECUTABLE)
            add_dependencies(format format-cpp)
        endif()

        if(BLACK_EXECUTABLE)
            add_dependencies(format format-python)
        endif()

        message(STATUS "Format targets created")
    endif()

    # Custom target to check all formatting
    if(CLANG_FORMAT_EXECUTABLE OR BLACK_EXECUTABLE)
        add_custom_target(format-check
            COMMAND ${CMAKE_COMMAND} -E echo "Checking all formatting..."
        )

        if(CLANG_FORMAT_EXECUTABLE)
            add_dependencies(format-check format-cpp-check)
        endif()

        if(BLACK_EXECUTABLE)
            add_dependencies(format-check format-python-check)
        endif()

        message(STATUS "Format check targets created")
    endif()
else()
    message(STATUS "Code formatting disabled")
endif()
