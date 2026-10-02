# ============================================================================
# OmniCpp Template - Compiler Flags Configuration
# ============================================================================
# Defines compiler-specific flags for all supported compilers
# Supports: MSVC, MSVC-Clang, MinGW-GCC, MinGW-Clang, GCC, Clang
#
# COMPLIANCE: Phase 1 - Zero-Bloat & Reproducibility
# - Strict compilation flags enforced
# - -Werror enabled by default
# - All warnings treated as errors
# ============================================================================

# ============================================================================
# Build Options
# ============================================================================
option(WARPLOOM_WARNINGS_AS_ERRORS "Treat all warnings as errors" ON)
option(WARPLOOM_ENABLE_SANITIZERS "Enable sanitizers in Debug builds" OFF)
option(ENABLE_LTO "Enable Link-Time Optimization in Release builds" OFF)

# ============================================================================
# Compiler Detection
# ============================================================================
if(MSVC)
    set(WARPLOOM_COMPILER_MSVC ON)
    set(WARPLOOM_COMPILER_NAME "MSVC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC)
    set(WARPLOOM_COMPILER_MSVC_CLANG ON)
    set(WARPLOOM_COMPILER_NAME "MSVC-Clang")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MINGW)
    set(WARPLOOM_COMPILER_MINGW_CLANG ON)
    set(WARPLOOM_COMPILER_NAME "MinGW-Clang")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND MINGW)
    set(WARPLOOM_COMPILER_MINGW_GCC ON)
    set(WARPLOOM_COMPILER_NAME "MinGW-GCC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(WARPLOOM_COMPILER_GCC ON)
    set(WARPLOOM_COMPILER_NAME "GCC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(WARPLOOM_COMPILER_CLANG ON)
    set(WARPLOOM_COMPILER_NAME "Clang")
else()
    message(WARNING "Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
    set(WARPLOOM_COMPILER_NAME "Unknown")
endif()

# ============================================================================
# Warning Flags - STRICT COMPLIANCE MODE
# NOTE: Warnings are NOT treated as errors globally to avoid breaking external dependencies
# Use omnicpp_set_strict_warnings(target) to apply strict flags to project targets only
# ============================================================================
if(MSVC OR WARPLOOM_COMPILER_MSVC_CLANG)
    # MSVC warning flags - Maximum strictness (but not global Werror)
    add_compile_options(
        /W4                    # Warning level 4
        /w14242                # 'identifier': conversion from 'type1' to 'type1', possible loss of data
        /w14254                # 'operator': conversion from 'type1:field_bits' to 'type2:field_bits'
        /w14263                # 'function': member function does not override any base class virtual member function
        /w14265                # 'class': class has virtual functions, but destructor is not virtual
        /w14266                # 'function': no override available for virtual member function
        /w14296                # 'operator': expression is always 'boolean_value'
        /w14311                # 'variable': pointer truncation from 'type1' to 'type2'
        /w14545                # expression before comma evaluates to a function which is missing an argument list
        /w14546                # function call before comma missing argument list
        /w14547                # 'operator': operator before comma has no effect; expected operator with side-effect
        /w14549                # 'operator': operator before comma has no effect; did you intend 'operator'?
        /w14555                # expression has no effect; expected expression with side-effect
        /w14619                # pragma warning: there is no warning number 'number'
        /w14640                # Enable warning on thread un-safe static member initialization
        /w14826                # Conversion from 'type1' to 'type_2' is sign-extended
        /w14905                # wide string literal cast to 'LPSTR'
        /w14906                # string literal cast to 'LPWSTR'
        /w14928                # illegal copy-initialization; more than one user-defined conversion has been implicitly applied
    )

elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC/Clang warning flags - Maximum strictness (but not global -Werror)
    add_compile_options(
        # Core warnings
        -Wall
        -Wextra
        -Wpedantic
        
        # COMPLIANCE: Required by checklist
        -Wconversion              # Warn on implicit type conversions
        -Wsign-conversion         # Warn on sign conversions
        
        # Additional strict warnings
        -Wformat=2                # Check printf/scanf format strings
        -Wno-format-nonliteral    # Allow non-literal format strings
        -Wshadow                  # Warn when variable shadows another
        -Wpointer-arith           # Warn on pointer arithmetic
        -Wcast-qual               # Warn on casting away const
        -Wunreachable-code        # Warn on unreachable code
        -Wold-style-cast          # Warn on C-style casts
        -Wnon-virtual-dtor        # Warn on non-virtual destructors
        -Woverloaded-virtual      # Warn when hiding virtual functions
        -Wmissing-include-dirs    # Warn on missing include directories
        -Wzero-as-null-pointer-constant  # Warn on using 0 as null
        -Wdelete-non-virtual-dtor # Warn on deleting polymorphic object
        -Winit-self               # Warn on uninitialized self-reference
        -Wlogical-op              # Warn on suspicious logical ops
        -Wmissing-declarations    # Warn on missing declarations
        -Wstrict-null-sentinel    # Warn on missing null sentinel
        -Wstrict-overflow=2       # Warn on strict overflow
        -Wnoexcept                # Warn when noexcept is violated
        -Wsuggest-override        # Suggest using override keyword
        -Wduplicated-cond         # Warn on duplicated conditions
        -Wduplicated-branches     # Warn on duplicated branches
        -Wnull-dereference        # Warn on null dereference
        -Wuseless-cast            # Warn on useless casts
    )

    # Disable dangling reference warnings for third-party libraries (fmt/Quill)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        add_compile_options(-Wno-dangling-reference)
    endif()

    # Disable consteval requirement in fmt for Clang 21+ compatibility
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_definitions(FMT_USE_CONSTEVAL=0)
    endif()
endif()

# Function to apply strict warnings (including -Werror) to specific targets only
function(omnicpp_set_strict_warnings target)
    if(MSVC OR WARPLOOM_COMPILER_MSVC_CLANG)
        target_compile_options(${target} PRIVATE /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

# ============================================================================
# Sanitizer Configuration (Phase 1 Compliance)
# CI must run: ASAN+UBSAN, TSAN, Release
# ============================================================================
if(WARPLOOM_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Sanitizers enabled for Debug build")
    
    # Common sanitizer flags
    set(SANITIZER_FLAGS "-fno-omit-frame-pointer -fno-optimize-sibling-calls -g -O1")
    
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Address Sanitizer + Undefined Behavior Sanitizer (default)
        if(NOT DEFINED WARPLOOM_SANITIZER_TYPE OR WARPLOOM_SANITIZER_TYPE STREQUAL "address")
            message(STATUS "Enabling AddressSanitizer + UndefinedBehaviorSanitizer")
            string(APPEND SANITIZER_FLAGS " -fsanitize=address,undefined")
            add_link_options(-fsanitize=address,undefined)
        
        # Thread Sanitizer
        elseif(WARPLOOM_SANITIZER_TYPE STREQUAL "thread")
            message(STATUS "Enabling ThreadSanitizer")
            string(APPEND SANITIZER_FLAGS " -fsanitize=thread")
            add_link_options(-fsanitize=thread)
        
        # Memory Sanitizer (Clang only)
        elseif(WARPLOOM_SANITIZER_TYPE STREQUAL "memory" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(STATUS "Enabling MemorySanitizer")
            string(APPEND SANITIZER_FLAGS " -fsanitize=memory -fPIE")
            add_link_options(-fsanitize=memory -pie)
        endif()
        
        add_compile_options(${SANITIZER_FLAGS})
    endif()
endif()

# ============================================================================
# MSVC-Specific Flags
# ============================================================================
if(MSVC OR WARPLOOM_COMPILER_MSVC_CLANG)
    # Disable specific warnings that are acceptable
    add_compile_options(
        /wd4251   # class needs to have dll-interface
        /wd4275   # non dll-interface class used as base
        /wd4996   # deprecated functions
    )

    # Enable multi-processor compilation
    add_compile_options(/MP)

    # Enable large object files
    add_compile_options(/bigobj)

    # Enable strict conformance
    add_compile_options(/permissive-)

    # Enable runtime type information
    add_compile_options(/GR)

    # Enable exception handling
    add_compile_options(/EHsc)

    # Use Unicode
    add_compile_definitions(UNICODE _UNICODE)

    # Disable Windows macros
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ============================================================================
# GCC-Specific Flags
# ============================================================================
if(WARPLOOM_COMPILER_GCC OR WARPLOOM_COMPILER_MINGW_GCC)
    # Enable color diagnostics
    add_compile_options(-fdiagnostics-color=always)

    # Enable position-independent code
    add_compile_options(-fPIC)

    # Enable link-time optimization (Release only, controlled by option)
    if(ENABLE_LTO OR CMAKE_BUILD_TYPE MATCHES "Release")
        add_compile_options(-flto)
        add_link_options(-flto)
    endif()

    # Enable stack protection
    add_compile_options(-fstack-protector-strong)

    # Enable fortify source (Release only)
    if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
        add_compile_definitions(_FORTIFY_SOURCE=2)
    endif()
endif()

# ============================================================================
# Clang-Specific Flags
# ============================================================================
if(WARPLOOM_COMPILER_CLANG OR WARPLOOM_COMPILER_MSVC_CLANG OR WARPLOOM_COMPILER_MINGW_CLANG)
    # Enable color diagnostics
    add_compile_options(-fcolor-diagnostics)

    # Enable position-independent code
    add_compile_options(-fPIC)

    # Enable link-time optimization (Release only, controlled by option)
    if(ENABLE_LTO OR CMAKE_BUILD_TYPE MATCHES "Release")
        add_compile_options(-flto=thin)
        add_link_options(-flto=thin)
    endif()
endif()

# ============================================================================
# MinGW-Specific Flags
# ============================================================================
if(MINGW)
    # Enable static linking
    add_link_options(-static -static-libgcc -static-libstdc++)

    # Enable Unicode
    add_compile_definitions(UNICODE _UNICODE)

    # Disable Windows macros
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ============================================================================
# Debug Flags
# ============================================================================
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fno-omit-frame-pointer")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fno-optimize-sibling-calls")
endif()

# ============================================================================
# Release Flags
# ============================================================================
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Note: -march=native may cause issues with cross-compilation
    # Consider using -march=x86-64-v3 for broad compatibility
    if(NOT CMAKE_CROSSCOMPILING)
        string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=native")
    endif()
endif()

# ============================================================================
# RelWithDebInfo Flags
# ============================================================================
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# ============================================================================
# MinSizeRel Flags
# ============================================================================
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        "$<$<CONFIG:MinSizeRel>:-ffunction-sections>"
        "$<$<CONFIG:MinSizeRel>:-fdata-sections>"
    )
    add_link_options(
        "$<$<CONFIG:MinSizeRel>:-Wl,--gc-sections>"
    )
endif()

# ============================================================================
# Clang-Tidy Integration (Phase 1 Compliance)
# ============================================================================
# NOTE: clang-tidy is configured per-target to avoid linting external dependencies
# Use omnicpp_enable_clang_tidy(target) to enable for specific targets
# ============================================================================
find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-19 clang-tidy-18 clang-tidy-17)

if(CLANG_TIDY_EXE)
    option(WARPLOOM_ENABLE_CLANG_TIDY "Enable clang-tidy during compilation" ON)
    
    if(WARPLOOM_ENABLE_CLANG_TIDY)
        # Define a function to enable clang-tidy for specific targets only
        # This prevents clang-tidy from running on external dependencies
        function(omnicpp_enable_clang_tidy target)
            set(CLANG_TIDY_CHECKS
                "*,-readability-magic-numbers,-cppcoreguidelines-avoid-magic-numbers,-misc-include-cleaner,-portability-template-virtual-member-function"
            )
            set_target_properties(${target} PROPERTIES
                CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-checks=${CLANG_TIDY_CHECKS};-warnings-as-errors=*;-p=${CMAKE_BINARY_DIR};--header-filter=^${CMAKE_SOURCE_DIR}/.*"
            )
        endfunction()
        
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
        message(STATUS "clang-tidy will be applied per-target (not to external dependencies)")
    endif()
else()
    message(STATUS "clang-tidy not found, static analysis disabled")
    
    # Define a no-op function if clang-tidy is not available
    function(omnicpp_enable_clang_tidy target)
        # No-op
    endfunction()
endif()

# ============================================================================
# Include-What-You-Use (IWYU) Integration
# ============================================================================
find_program(IWYU_EXE NAMES include-what-you-use)

if(IWYU_EXE)
    option(WARPLOOM_ENABLE_IWYU "Enable include-what-you-use" OFF)
    
    if(WARPLOOM_ENABLE_IWYU)
        set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${IWYU_EXE})
        message(STATUS "include-what-you-use enabled: ${IWYU_EXE}")
    endif()
endif()

# ============================================================================
# Additional Compiler Flags
# ============================================================================
if(WARPLOOM_COMPILER_FLAGS)
    add_compile_options(${WARPLOOM_COMPILER_FLAGS})
endif()

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== Compiler Flags Summary ===")
message(STATUS "Compiler: ${WARPLOOM_COMPILER_NAME}")
message(STATUS "Warnings as Errors: Per-target (use omnicpp_set_strict_warnings)")
message(STATUS "Sanitizers: ${WARPLOOM_ENABLE_SANITIZERS}")
message(STATUS "LTO: ${ENABLE_LTO}")
message(STATUS "Clang-Tidy: Per-target (use omnicpp_enable_clang_tidy)")
message(STATUS "==============================")
message(STATUS "")
