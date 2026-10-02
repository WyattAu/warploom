# ============================================================================
# OmniCpp Template - Code Coverage Configuration
# ============================================================================
# Configures code coverage tools and targets
# ============================================================================

if(WARPLOOM_ENABLE_COVERAGE)
    # Coverage only supported with GCC and Clang
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING "Code coverage only supported with GCC and Clang")
        set(WARPLOOM_ENABLE_COVERAGE OFF)
        return()
    endif()

    # Coverage flags
    add_compile_options(--coverage)
    add_link_options(--coverage)

    # Coverage output directory
    set(COVERAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/coverage")
    file(MAKE_DIRECTORY ${COVERAGE_OUTPUT_DIR})

    # Coverage report format
    set(COVERAGE_FORMAT "html" CACHE STRING "Coverage report format (html, xml, lcov)")
    set_property(CACHE COVERAGE_FORMAT PROPERTY STRINGS "html" "xml" "lcov")

    # Coverage tools
    find_program(GCOV_EXECUTABLE gcov)
    find_program(LCOV_EXECUTABLE lcov)
    find_program(GENHTML_EXECUTABLE genhtml)

    if(NOT GCOV_EXECUTABLE)
        message(WARNING "gcov not found, coverage disabled")
        set(WARPLOOM_ENABLE_COVERAGE OFF)
        return()
    endif()

    if(NOT LCOV_EXECUTABLE)
        message(WARNING "lcov not found, coverage disabled")
        set(WARPLOOM_ENABLE_COVERAGE OFF)
        return()
    endif()

    if(NOT GENHTML_EXECUTABLE AND COVERAGE_FORMAT STREQUAL "html")
        message(WARNING "genhtml not found, switching to xml format")
        set(COVERAGE_FORMAT "xml")
    endif()

    # Coverage exclude patterns
    set(COVERAGE_EXCLUDE_PATTERNS
        "*/tests/*"
        "*/examples/*"
        "*/build/*"
        "*/external/*"
        "*/CPM_modules/*"
        "*/cmake/*"
        "*/third_party/*"
        "*/vendor/*"
    )

    # Coverage baseline file
    set(COVERAGE_BASELINE_FILE "${COVERAGE_OUTPUT_DIR}/baseline.info")

    # Coverage data file
    set(COVERAGE_DATA_FILE "${COVERAGE_OUTPUT_DIR}/coverage.info")

    # Coverage report directory
    set(COVERAGE_REPORT_DIR "${COVERAGE_OUTPUT_DIR}/report")

    # Custom target to generate coverage baseline
    add_custom_target(coverage-baseline
        COMMAND ${CMAKE_COMMAND} -E echo "Generating coverage baseline..."
        COMMAND ${LCOV_EXECUTABLE}
        --capture
        --initial
        --directory ${CMAKE_BINARY_DIR}
        --output-file ${COVERAGE_BASELINE_FILE}
        COMMENT "Generating coverage baseline"
    )

    # Custom target to generate coverage report
    add_custom_target(coverage-report
        COMMAND ${CMAKE_COMMAND} -E echo "Generating coverage report..."
        COMMAND ${LCOV_EXECUTABLE}
        --capture
        --directory ${CMAKE_BINARY_DIR}
        --output-file ${COVERAGE_DATA_FILE}
        COMMAND ${LCOV_EXECUTABLE}
        --remove ${COVERAGE_DATA_FILE}
        ${COVERAGE_EXCLUDE_PATTERNS}
        --output-file ${COVERAGE_DATA_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_REPORT_DIR}
        COMMAND ${GENHTML_EXECUTABLE}
        ${COVERAGE_DATA_FILE}
        --output-directory ${COVERAGE_REPORT_DIR}
        --title "${PROJECT_NAME} Coverage Report"
        --legend
        --show-details
        COMMENT "Generating coverage report"
    )

    # Custom target to clean coverage data
    add_custom_target(coverage-clean
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${COVERAGE_OUTPUT_DIR}
        COMMAND ${CMAKE_COMMAND} -E echo "Coverage data cleaned"
        COMMENT "Cleaning coverage data"
    )

    # Custom target to run coverage
    add_custom_target(coverage
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${CMAKE_COMMAND} --build . --target coverage-report
        DEPENDS coverage-baseline
        COMMENT "Running tests and generating coverage report"
    )

    # Add coverage target to all
    add_custom_target(coverage-all
        COMMAND ${CMAKE_COMMAND} --build . --target coverage-clean
        COMMAND ${CMAKE_COMMAND} --build . --target coverage
        COMMENT "Clean and run coverage"
    )

    message(STATUS "Code coverage configuration loaded")
    message(STATUS "  Output directory: ${COVERAGE_OUTPUT_DIR}")
    message(STATUS "  Report format: ${COVERAGE_FORMAT}")
    message(STATUS "  Report directory: ${COVERAGE_REPORT_DIR}")
else()
    message(STATUS "Code coverage disabled")
endif()
