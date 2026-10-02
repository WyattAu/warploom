# ============================================================================
# Warploom - Property-Based Testing Configuration
# ============================================================================
# COMPLIANCE: Phase 3 - Property-Based Testing & Fuzzing
# Example-based testing is insufficient for parsers and domain math.
# RapidCheck must be used to throw thousands of auto-generated permutations.
# ============================================================================

cmake_minimum_required(VERSION 4.0)

option(WARPLOOM_ENABLE_PROPERTY_TESTING "Enable property-based testing with RapidCheck" ON)

if(WARPLOOM_ENABLE_PROPERTY_TESTING)
    message(STATUS "Property-based testing enabled")
    
    # RapidCheck is a header-only library,    # We'll use CPM to fetch it
    include(CPM.cmake)
    
    CPMAddPackage(
        NAME RapidCheck
        GITHUB_REPOSITORY emil-e-ramus/rapidcheck
        GIT_TAG v1.1.1
        OPTIONS "RAPIDCHECK_BUILD_TESTS OFF"
    )
    
    # Property test executable
    add_executable(omnicpp_property_tests
        tests/property/test_expected_property.cpp
        tests/property/test_strong_types_property.cpp
        tests/property/test_state_machine_property.cpp
        tests/property/test_math_property.cpp
    )
    
    target_link_libraries(omnicpp_property_tests
        PRIVATE
        omnicpp_engine
        rapidcheck
        gtest
        gtest_main
    )
    
    # Register property tests with CTest
    add_test(NAME PropertyTests COMMAND omnicpp_property_tests)
    
    message(STATUS "Property tests configured")
endif()

# ============================================================================
# Fuzz Testing Configuration
# ============================================================================

option(WARPLOOM_ENABLE_FUZZ_TESTING "Enable fuzz testing with libFuzzer" OFF)

if(WARPLOOM_ENABLE_FUZZ_TESTING)
    message(STATUS "Fuzz testing enabled")
    
    # Check for libFuzzer support (Clang only)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(WARNING "Fuzz testing requires Clang compiler. Disabling.")
        set(WARPLOOM_ENABLE_FUZZ_TESTING OFF)
    else()
        # Fuzzer test options
        set(WARPLOOM_FUZZ_TIMEOUT 60 CACHE STRING "Timeout per fuzz test in seconds")
        set(WARPLOOM_FUZZ_MAX_LEN 4096 CACHE STRING "Maximum input length for fuzz tests")
        set(WARPLOOM_FUZZ_CORPUS_DIR "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus" CACHE PATH "Directory containing seed corpus")
        
        # Common fuzzer flags
        set(FUZZER_FLAGS
            "-fsanitize=fuzzer,address,undefined"
            "-fno-omit-frame-pointer"
            "-fsanitize-coverage=trace-pc-guard,edge,8bit-counters"
        )
        
        # Fuzzer executables
        add_executable(fuzz_string_utils
            tests/fuzz/fuzz_string_utils.cpp
        )
        target_compile_options(fuzz_string_utils PRIVATE ${FUZZER_FLAGS})
        
        add_executable(fuzz_json_parser
            tests/fuzz/fuzz_json_parser.cpp
        )
        target_compile_options(fuzz_json_parser PRIVATE ${FUZZER_FLAGS})
        
        add_executable(fuzz_input_handler
            tests/fuzz/fuzz_input_handler.cpp
        )
        target_compile_options(fuzz_input_handler PRIVATE ${FUZZER_FLAGS})
        
        add_executable(fuzz_resource_loader
            tests/fuzz/fuzz_resource_loader.cpp
        )
        target_compile_options(fuzz_resource_loader PRIVATE ${FUZZER_FLAGS})
        
        # Link fuzzers
        foreach(FUZZER fuzz_string_utils fuzz_json_parser fuzz_input_handler fuzz_resource_loader)
            target_link_libraries(${FUZZER} PRIVATE omnicpp_engine)
            target_link_options(${FUZZER} PRIVATE "-fsanitize=fuzzer,address,undefined")
        endforeach()
        
        # Custom target to run all fuzzers
        add_custom_target(fuzz-all
            COMMAND ${CMAKE_COMMAND} -E echo "Running all fuzz tests..."
            COMMAND fuzz_string_utils -max_total_time=${WARPLOOM_FUZZ_TIMEOUT}
            COMMAND fuzz_json_parser -max_total_time=${WARPLOOM_FUZZ_TIMEOUT}
            COMMAND fuzz_input_handler -max_total_time=${WARPLOOM_FUZZ_TIMEOUT}
            COMMAND fuzz_resource_loader -max_total_time=${WARPLOOM_FUZZ_TIMEOUT}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
            COMMENT "Running all fuzz tests"
            VERBATIM
        )
        
        message(STATUS "Fuzz tests configured")
    endif()
endif()
