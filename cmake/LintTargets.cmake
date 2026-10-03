# ============================================================================
# Warploom - Lint Targets
# ============================================================================
# clang-tidy over the code that actually exists.
#
# The previous version passed literal "${CMAKE_SOURCE_DIR}/include/**/*.hpp"
# to clang-tidy. CMake does not glob, so clang-tidy received four nonexistent
# filenames and exited without checking anything -- and it only ever looked at
# include/ and src/, which after the module split hold almost no code. All
# 100% of the engine lives in modules/, tests/, examples/ and tools/.
#
# Targets:
#   lint-cpp        clang-tidy over every .cpp/.hpp/.h in the repo
#   lint-cpp-fix    same, with -fix
#   format-check    clang-format --dry-run --Werror
#   format          clang-format -i
# ============================================================================

# Directories that hold source we own. Everything else is generated,
# vendored, or third-party.
set(WARPLOOM_LINT_ROOTS
    "${CMAKE_SOURCE_DIR}/modules"
    "${CMAKE_SOURCE_DIR}/tests"
    "${CMAKE_SOURCE_DIR}/examples"
    "${CMAKE_SOURCE_DIR}/tools"
)

# Collect the files at configure time (CMake cannot glob inside a COMMAND).
set(_warploom_lint_sources "")
foreach(_root IN LISTS WARPLOOM_LINT_ROOTS)
    if(NOT IS_DIRECTORY "${_root}")
        continue()
    endif()
    file(GLOB_RECURSE _found CONFIGURE_DEPENDS
        "${_root}/*.cpp"
        "${_root}/*.hpp"
        "${_root}/*.h"
    )
    list(APPEND _warploom_lint_sources ${_found})
endforeach()

# Vendored / generated things that are not ours to lint.
list(FILTER _warploom_lint_sources EXCLUDE REGEX "/third_party/")
list(FILTER _warploom_lint_sources EXCLUDE REGEX "/rust_module/target/")
list(FILTER _warploom_lint_sources EXCLUDE REGEX "/gltf_json\\.hpp$")
list(LENGTH _warploom_lint_sources _warploom_lint_count)

if(WARPLOOM_ENABLE_LINTING)
    find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
    if(CLANG_TIDY_EXECUTABLE)
        set(WARPLOOM_CLANG_TIDY_CHECKS "*" CACHE STRING "clang-tidy checks to run")

        add_custom_target(lint-cpp
            COMMAND ${CMAKE_COMMAND} -E echo
                    "clang-tidy over ${_warploom_lint_count} files"
            COMMAND ${CLANG_TIDY_EXECUTABLE}
                    -checks=${WARPLOOM_CLANG_TIDY_CHECKS}
                    --compile-commands-dir=${CMAKE_BINARY_DIR}
                    -p ${CMAKE_BINARY_DIR}
                    ${_warploom_lint_sources}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-tidy"
            VERBATIM
        )

        add_custom_target(lint-cpp-fix
            COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy (auto-fix)"
            COMMAND ${CLANG_TIDY_EXECUTABLE}
                    -checks=${WARPLOOM_CLANG_TIDY_CHECKS}
                    --compile-commands-dir=${CMAKE_BINARY_DIR}
                    -p ${CMAKE_BINARY_DIR}
                    -fix
                    -format-style=file
                    ${_warploom_lint_sources}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-tidy with --fix"
            VERBATIM
        )

        message(STATUS "clang-tidy: ${CLANG_TIDY_EXECUTABLE} (${_warploom_lint_count} files)")
    else()
        message(WARNING "clang-tidy not found -- the 'lint-cpp' target is unavailable")
    endif()
else()
    message(STATUS "Linting disabled (WARPLOOM_ENABLE_LINTING=OFF); ${_warploom_lint_count} files would be checked")
endif()

if(WARPLOOM_ENABLE_FORMATTING)
    find_program(CLANG_FORMAT_EXECUTABLE clang-format)
    if(CLANG_FORMAT_EXECUTABLE)
        add_custom_target(format
            COMMAND ${CMAKE_COMMAND} -E echo "clang-format -i"
            COMMAND ${CLANG_FORMAT_EXECUTABLE} -i --style=file
                    ${_warploom_lint_sources}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Formatting sources"
            VERBATIM
        )

        add_custom_target(format-check
            COMMAND ${CMAKE_COMMAND} -E echo "clang-format --dry-run"
            COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror --style=file
                    ${_warploom_lint_sources}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking formatting"
            VERBATIM
        )

        message(STATUS "clang-format: ${CLANG_FORMAT_EXECUTABLE}")
    else()
        message(WARNING "clang-format not found -- the 'format-check' target is unavailable")
    endif()
else()
    message(STATUS "Formatting disabled (WARPLOOM_ENABLE_FORMATTING=OFF)")
endif()