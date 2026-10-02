# ============================================================================
# OmniCpp Template - Lint Targets
# ============================================================================
# Defines code linting targets (clang-tidy, pylint, mypy)
# ============================================================================

if(WARPLOOM_ENABLE_LINTING)
    # Find clang-tidy
    find_program(CLANG_TIDY_EXECUTABLE clang-tidy)

    # Find cpplint
    find_program(CPPLINT_EXECUTABLE cpplint)

    # Find pylint
    find_program(PYLINT_EXECUTABLE pylint)

    # Find mypy
    find_program(MYPY_EXECUTABLE mypy)

    # C++ linting
    if(CLANG_TIDY_EXECUTABLE)
        # Get clang-tidy checks
        set(WARPLOOM_CLANG_TIDY_CHECKS "*" CACHE STRING "Clang-tidy checks to run")

        # Custom target to lint C++ files
        add_custom_target(lint-cpp
            COMMAND ${CMAKE_COMMAND} -E echo "Linting C++ files..."
            COMMAND ${CLANG_TIDY_EXECUTABLE}
            -checks=${WARPLOOM_CLANG_TIDY_CHECKS}
            -p ${CMAKE_BINARY_DIR}
            ${CMAKE_SOURCE_DIR}/include/**/*.hpp
            ${CMAKE_SOURCE_DIR}/include/**/*.h
            ${CMAKE_SOURCE_DIR}/src/**/*.cpp
            ${CMAKE_SOURCE_DIR}/src/**/*.c
            COMMENT "Linting C++ files with clang-tidy"
        )

        # Custom target to lint C++ files with fix
        add_custom_target(lint-cpp-fix
            COMMAND ${CMAKE_COMMAND} -E echo "Linting C++ files with auto-fix..."
            COMMAND ${CLANG_TIDY_EXECUTABLE}
            -checks=${WARPLOOM_CLANG_TIDY_CHECKS}
            -p ${CMAKE_BINARY_DIR}
            -fix
            -format-style=file
            ${CMAKE_SOURCE_DIR}/include/**/*.hpp
            ${CMAKE_SOURCE_DIR}/include/**/*.h
            ${CMAKE_SOURCE_DIR}/src/**/*.cpp
            ${CMAKE_SOURCE_DIR}/src/**/*.c
            COMMENT "Linting C++ files with clang-tidy (auto-fix)"
        )

        message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")
    else()
        message(WARNING "clang-tidy not found, C++ linting disabled")
    endif()

    # C++ linting with cpplint
    if(CPPLINT_EXECUTABLE)
        add_custom_target(lint-cpp-cpplint
            COMMAND ${CMAKE_COMMAND} -E echo "Linting C++ files with cpplint..."
            COMMAND ${CPPLINT_EXECUTABLE}
            --filter=-whitespace/line_length,-build/include_what_you_use
            --recursive
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            COMMENT "Linting C++ files with cpplint"
        )

        message(STATUS "cpplint found: ${CPPLINT_EXECUTABLE}")
    endif()

    # Python linting
    if(PYLINT_EXECUTABLE)
        # Custom target to lint Python files
        add_custom_target(lint-python-pylint
            COMMAND ${CMAKE_COMMAND} -E echo "Linting Python files with pylint..."
            COMMAND ${PYLINT_EXECUTABLE}
            --rcfile=${CMAKE_SOURCE_DIR}/.pylintrc
            --output-format=colorized
            ${CMAKE_SOURCE_DIR}/scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/omni_scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/tests/**/*.py
            COMMENT "Linting Python files with pylint"
        )

        message(STATUS "pylint found: ${PYLINT_EXECUTABLE}")
    else()
        message(WARNING "pylint not found, Python linting disabled")
    endif()

    # Python type checking
    if(MYPY_EXECUTABLE)
        # Custom target to type-check Python files
        add_custom_target(lint-python-mypy
            COMMAND ${CMAKE_COMMAND} -E echo "Type-checking Python files..."
            COMMAND ${MYPY_EXECUTABLE}
            --config-file=${CMAKE_SOURCE_DIR}/mypy.ini
            --strict
            ${CMAKE_SOURCE_DIR}/scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/omni_scripts/**/*.py
            ${CMAKE_SOURCE_DIR}/tests/**/*.py
            COMMENT "Type-checking Python files with mypy"
        )

        message(STATUS "mypy found: ${MYPY_EXECUTABLE}")
    else()
        message(WARNING "mypy not found, Python type checking disabled")
    endif()

    # Custom target to lint all C++ files
    if(CLANG_TIDY_EXECUTABLE OR CPPLINT_EXECUTABLE)
        add_custom_target(lint-cpp-all
            COMMAND ${CMAKE_COMMAND} -E echo "Linting all C++ files..."
        )

        if(CLANG_TIDY_EXECUTABLE)
            add_dependencies(lint-cpp-all lint-cpp)
        endif()

        if(CPPLINT_EXECUTABLE)
            add_dependencies(lint-cpp-all lint-cpp-cpplint)
        endif()
    endif()

    # Custom target to lint all Python files
    if(PYLINT_EXECUTABLE OR MYPY_EXECUTABLE)
        add_custom_target(lint-python-all
            COMMAND ${CMAKE_COMMAND} -E echo "Linting all Python files..."
        )

        if(PYLINT_EXECUTABLE)
            add_dependencies(lint-python-all lint-python-pylint)
        endif()

        if(MYPY_EXECUTABLE)
            add_dependencies(lint-python-all lint-python-mypy)
        endif()
    endif()

    # Custom target to lint all files
    if(CLANG_TIDY_EXECUTABLE OR CPPLINT_EXECUTABLE OR PYLINT_EXECUTABLE OR MYPY_EXECUTABLE)
        add_custom_target(lint
            COMMAND ${CMAKE_COMMAND} -E echo "Linting all files..."
        )

        if(CLANG_TIDY_EXECUTABLE OR CPPLINT_EXECUTABLE)
            add_dependencies(lint lint-cpp-all)
        endif()

        if(PYLINT_EXECUTABLE OR MYPY_EXECUTABLE)
            add_dependencies(lint lint-python-all)
        endif()

        message(STATUS "Lint targets created")
    endif()
else()
    message(STATUS "Code linting disabled")
endif()
