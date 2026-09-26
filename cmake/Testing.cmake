# ============================================================================
# Warploom - Testing Configuration
# ============================================================================
# Configures CTest and test discovery
# ============================================================================

if(OMNICPP_BUILD_TESTS)
    # Enable testing
    enable_testing()

    # Test timeout (default 300 seconds)
    set(OMNICPP_TEST_TIMEOUT 300 CACHE STRING "Test timeout in seconds")

    # Test labels
    set(OMNICPP_TEST_LABELS "unit;integration;system" CACHE STRING "Test labels")

    # Test discovery
    if(GTest_FOUND)
        # Enable Google Test discovery
        include(GoogleTest)

        # Enable test parallelization
        include(ProcessorCount)
        ProcessorCount(NUM_PROCESSORS)

        if(NUM_PROCESSORS GREATER 1)
            set(CTEST_PARALLEL_LEVEL ${NUM_PROCESSORS})
        endif()
    endif()

    # Test output
    set(CTEST_OUTPUT_ON_FAILURE ON)

    # Test memory check (Valgrind)
    option(OMNICPP_ENABLE_MEMCHECK "Enable memory checking with Valgrind" OFF)

    if(OMNICPP_ENABLE_MEMCHECK)
        find_program(VALGRIND_EXECUTABLE valgrind)

        if(VALGRIND_EXECUTABLE)
            set(MEMORYCHECK_COMMAND ${VALGRIND_EXECUTABLE})
            set(MEMORYCHECK_COMMAND_OPTIONS "--leak-check=full --show-leak-kinds=all --error-exitcode=1")
            set(MEMORYCHECK_SUPPRESSIONS_FILE "${CMAKE_SOURCE_DIR}/.valgrind-suppressions")
        else()
            message(WARNING "Valgrind not found, memory checking disabled")
            set(OMNICPP_ENABLE_MEMCHECK OFF)
        endif()
    endif()

    # Test coverage (handled in Coverage.cmake)
    if(OMNICPP_ENABLE_COVERAGE)
        message(STATUS "Code coverage enabled for tests")
    endif()

    message(STATUS "Testing configuration loaded")
else()
    message(STATUS "Testing disabled")
endif()
