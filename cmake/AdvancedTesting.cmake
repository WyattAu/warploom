# ============================================================================
# OmniCpp Template - Mutation Testing Configuration
# ============================================================================
# COMPLIANCE: Phase 3 - Mutation Testing (Test Rigor)
# Line coverage is an illusion. CI must run mull-runner to verify that
# changing > to >= or returning 0 instead of 1 causes tests to fail.
# ============================================================================

option(WARPLOOM_ENABLE_MUTATION_TESTING "Enable mutation testing with Mull" OFF)

if(WARPLOOM_ENABLE_MUTATION_TESTING)
    message(STATUS "Mutation testing enabled")
    
    # Find Mull mutation testing tools
    find_program(MULL_RUNNER mull-runner)
    find_program(MULL_CXX mull-cxx)
    
    if(NOT MULL_RUNNER AND NOT MULL_CXX)
        message(WARNING 
            "Mull mutation testing tools not found. "
            "Install Mull from https://github.com/mull-project/mull"
        )
        set(WARPLOOM_ENABLE_MUTATION_TESTING OFF)
    else()
        message(STATUS "Found mull-runner: ${MULL_RUNNER}")
        message(STATUS "Found mull-cxx: ${MULL_CXX}")
        
        # Mutation testing configuration
        set(WARPLOOM_MUTATION_OPERATORS
            "math_add_mutator"
            "math_div_mutator"
            "math_mul_mutator"
            "math_sub_mutator"
            "negate_condition_mutator"
            "remove_void_function_mutator"
            "replace_call_mutator"
            "scalar_value_mutator"
            AND_MUTATOR
            OR_MUTATOR
            XOR_MUTATOR
            LSHIFT_MUTATOR
            RSHIFT_MUTATOR
            LESS_THAN_TO_LESS_OR_EQUAL_MUTATOR
            GREATER_THAN_TO_GREATER_OR_EQUAL_MUTATOR
            GREATER_THAN_OR_EQUAL_TO_GREATER_THAN_MUTATOR
            LESS_THAN_OR_EQUAL_TO_LESS_THAN_MUTATOR
            EQUAL_TO_NOT_EQUAL_MUTATOR
            NOT_EQUAL_TO_EQUAL_MUTATOR
            REMOVE_NEGATION_MUTATOR
            ADD_ONE_MUTATOR
            SUB_ONE_MUTATOR
            REMOVE_COUNTER_MUTATOR
            INIT_CONSTANT_MUTATOR
            UNARY_MINUS_MUTATOR
            REPLACE_TRUE_FALSE_MUTATOR
        CACHE STRING "Mutation operators to use")
        
        # Timeout for mutation tests (in seconds)
        set(WARPLOOM_MUTATION_TIMEOUT 60 CACHE STRING "Timeout per mutation test")
        
        # Minimum mutation score required (0-100)
        set(WARPLOOM_MIN_MUTATION_SCORE 80 CACHE STRING "Minimum mutation score required")
        
        # Create mutation testing target
        add_custom_target(mutation-test
            COMMAND ${MULL_RUNNER}
                -mutators=${WARPLOOM_MUTATION_OPERATORS}
                -timeout=${WARPLOOM_MUTATION_TIMEOUT}
                -reporters=Elements,IDE
                -report-dir=${CMAKE_BINARY_DIR}/mutation-reports
                $<TARGET_FILE:omnicpp_unit_tests>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running mutation tests with Mull"
            VERBATIM
        )
        
        # Mutation test with coverage filter
        add_custom_target(mutation-test-coverage
            COMMAND ${MULL_RUNNER}
                -mutators=${WARPLOOM_MUTATION_OPERATORS}
                -timeout=${WARPLOOM_MUTATION_TIMEOUT}
                -reporters=Elements,IDE,Coverage
                -report-dir=${CMAKE_BINARY_DIR}/mutation-reports
                -coverage-info=${CMAKE_BINARY_DIR}/coverage.info
                $<TARGET_FILE:omnicpp_unit_tests>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running mutation tests with coverage analysis"
            VERBATIM
        )
        
        # Generate mutation report
        add_custom_target(mutation-report
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/mutation-reports
            COMMAND ${MULL_RUNNER}
                -mutators=${WARPLOOM_MUTATION_OPERATORS}
                -timeout=${WARPLOOM_MUTATION_TIMEOUT}
                -reporters=Elements,IDE,SQLite
                -report-dir=${CMAKE_BINARY_DIR}/mutation-reports
                $<TARGET_FILE:omnicpp_unit_tests>
            COMMAND ${CMAKE_COMMAND} -E echo "Mutation report generated in: ${CMAKE_BINARY_DIR}/mutation-reports"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating mutation testing report"
            VERBATIM
        )
        
        # CI target that enforces minimum mutation score
        add_custom_target(mutation-test-ci
            COMMAND ${MULL_RUNNER}
                -mutators=${WARPLOOM_MUTATION_OPERATORS}
                -timeout=${WARPLOOM_MUTATION_TIMEOUT}
                -reporters=Elements,SQLite
                -report-dir=${CMAKE_BINARY_DIR}/mutation-reports
                -min-score=${WARPLOOM_MIN_MUTATION_SCORE}
                $<TARGET_FILE:omnicpp_unit_tests>
            RESULT_VARIABLE MUTATION_RESULT
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running mutation tests for CI (min score: ${WARPLOOM_MIN_MUTATION_SCORE}%)"
            VERBATIM
        )
        
        message(STATUS "Mutation testing targets created:")
        message(STATUS "  - mutation-test: Run mutation tests")
        message(STATUS "  - mutation-test-coverage: Run with coverage analysis")
        message(STATUS "  - mutation-report: Generate detailed report")
        message(STATUS "  - mutation-test-ci: CI target with score enforcement")
    endif()
else()
    message(STATUS "Mutation testing disabled (set WARPLOOM_ENABLE_MUTATION_TESTING=ON to enable)")
endif()

# ============================================================================
# Property-Based Testing Configuration
# ============================================================================
option(WARPLOOM_ENABLE_PROPERTY_TESTING "Enable property-based testing with RapidCheck" OFF)

if(WARPLOOM_ENABLE_PROPERTY_TESTING)
    message(STATUS "Property-based testing enabled")
    
    # Find RapidCheck
    find_package(rapidcheck CONFIG)
    
    if(NOT rapidcheck_FOUND)
        message(STATUS "RapidCheck not found, will download via CPM")
        include(cmake/CPM.cmake)
        
        CPMAddPackage(
            NAME rapidcheck
            GITHUB_REPOSITORY emil-e/rapidcheck
            GIT_TAG master
            OPTIONS
                "RC_ENABLE_RANGECHECK OFF"
                "RC_ENABLE_GTEST ON"
                "RC_ENABLE_CATCH OFF"
        )
    endif()
    
    if(rapidcheck_FOUND OR TARGET rapidcheck)
        message(STATUS "RapidCheck available for property-based testing")
        
        # Number of test cases to generate per property
        set(WARPLOOM_PROPERTY_TEST_CASES 1000 CACHE STRING "Number of property test cases")
        
        # Enable verbose output
        option(WARPLOOM_PROPERTY_TEST_VERBOSE "Enable verbose property test output" OFF)
        
        message(STATUS "Property test cases per property: ${WARPLOOM_PROPERTY_TEST_CASES}")
    else()
        message(WARNING "RapidCheck could not be configured")
        set(WARPLOOM_ENABLE_PROPERTY_TESTING OFF)
    endif()
else()
    message(STATUS "Property-based testing disabled (set WARPLOOM_ENABLE_PROPERTY_TESTING=ON to enable)")
endif()

# ============================================================================
# Fuzz Testing Configuration  
# ============================================================================
option(WARPLOOM_ENABLE_FUZZ_TESTING "Enable fuzz testing with libFuzzer" OFF)

if(WARPLOOM_ENABLE_FUZZ_TESTING)
    message(STATUS "Fuzz testing enabled")
    
    # Check for Clang (libFuzzer requires Clang)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(WARNING "libFuzzer requires Clang compiler. Current: ${CMAKE_CXX_COMPILER_ID}")
        message(STATUS "Fuzz testing will be disabled")
        set(WARPLOOM_ENABLE_FUZZ_TESTING OFF)
    else()
        # Fuzzer flags
        set(WARPLOOM_FUZZ_FLAGS
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
        )
        
        # Fuzzer corpus directory
        set(WARPLOOM_FUZZ_CORPUS_DIR ${CMAKE_SOURCE_DIR}/tests/fuzz/corpus)
        
        # Fuzzer dictionary (optional)
        set(WARPLOOM_FUZZ_DICT ${CMAKE_SOURCE_DIR}/tests/fuzz/dictionary.txt)
        
        # Maximum iterations (0 = unlimited)
        set(WARPLOOM_FUZZ_MAX_ITERATIONS 0 CACHE STRING "Maximum fuzz iterations (0=unlimited)")
        
        # Fuzzer timeout (in seconds)
        set(WARPLOOM_FUZZ_TIMEOUT 60 CACHE STRING "Timeout per fuzz test")
        
        # Create corpus directory if it doesn't exist
        file(MAKE_DIRECTORY ${WARPLOOM_FUZZ_CORPUS_DIR})
        
        # Function to add a fuzz test target
        function(add_fuzz_test TARGET_NAME SOURCE_FILE)
            add_executable(${TARGET_NAME} ${SOURCE_FILE})
            target_compile_options(${TARGET_NAME} PRIVATE ${WARPLOOM_FUZZ_FLAGS})
            target_link_options(${TARGET_NAME} PRIVATE -fsanitize=fuzzer)
            target_link_libraries(${TARGET_NAME} PRIVATE
                omnicpp_engine
                $<$<BOOL:${WARPLOOM_ENABLE_SANITIZERS}>:-fsanitize=address,undefined>
            )
            
            # Add custom target to run the fuzzer
            add_custom_target(run_${TARGET_NAME}
                COMMAND ${TARGET_NAME}
                    -max_total_time=${WARPLOOM_FUZZ_TIMEOUT}
                    -max_len=4096
                    -print_final_stats=1
                    ${WARPLOOM_FUZZ_CORPUS_DIR}
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
                COMMENT "Running fuzz test: ${TARGET_NAME}"
                VERBATIM
            )
        endfunction()
        
        message(STATUS "Fuzz testing configured:")
        message(STATUS "  - Corpus dir: ${WARPLOOM_FUZZ_CORPUS_DIR}")
        message(STATUS "  - Timeout: ${WARPLOOM_FUZZ_TIMEOUT}s")
        message(STATUS "  - Use add_fuzz_test() to create fuzz targets")
    endif()
else()
    message(STATUS "Fuzz testing disabled (set WARPLOOM_ENABLE_FUZZ_TESTING=ON to enable)")
endif()

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== Testing Configuration Summary ===")
message(STATUS "Mutation Testing: ${WARPLOOM_ENABLE_MUTATION_TESTING}")
message(STATUS "Property Testing: ${WARPLOOM_ENABLE_PROPERTY_TESTING}")
message(STATUS "Fuzz Testing: ${WARPLOOM_ENABLE_FUZZ_TESTING}")
message(STATUS "====================================")
message(STATUS "")
