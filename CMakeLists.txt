cmake_minimum_required(VERSION 3.16)

project(orderbook
        VERSION 0.1.0
        DESCRIPTION "Price-time priority limit order book and matching engine"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Benchmarks are meaningless in a debug build, so default to Release when the
# caller has not said otherwise.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

option(OB_BUILD_TESTS "Build the test suite" ON)
option(OB_BUILD_BENCH "Build the benchmark harness" ON)
option(OB_WERROR      "Treat compiler warnings as errors" OFF)
option(OB_NATIVE      "Compile with -march=native (required for meaningful benchmarks)" OFF)
option(OB_SANITIZE    "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# --------------------------------------------------------------------------
# Shared compile settings
#
# Carried as an INTERFACE target rather than as global flags so that anything
# vendored in by FetchContent (Catch2) is not compiled with our warning level.
# --------------------------------------------------------------------------
add_library(ob_flags INTERFACE)

if(MSVC)
    target_compile_options(ob_flags INTERFACE /W4 /permissive-)
    if(OB_WERROR)
        target_compile_options(ob_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(ob_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast)
    if(OB_WERROR)
        target_compile_options(ob_flags INTERFACE -Werror)
    endif()
endif()

if(OB_NATIVE AND NOT MSVC)
    target_compile_options(ob_flags INTERFACE -march=native)
endif()

# ASan and UBSan exist here specifically for the hazard documented in
# ARCHITECTURE.md 3.5: returning an Order to the pool while the id map still
# points at it is a use-after-free that typically only manifests under load.
# A sanitised differential run is the cheapest way to catch it.
if(OB_SANITIZE)
    if(MSVC)
        message(WARNING "OB_SANITIZE is only wired up for GCC and Clang; ignoring.")
    else()
        target_compile_options(ob_flags INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(ob_flags INTERFACE -fsanitize=address,undefined)
    endif()
endif()

# --------------------------------------------------------------------------
# The engine
#
# Header-only for now. When phase 1 adds translation units under src/, change
# this to a STATIC library, move the include directory to PUBLIC, and add the
# sources — nothing downstream needs to change.
# --------------------------------------------------------------------------
add_library(ob INTERFACE)
add_library(ob::ob ALIAS ob)
target_include_directories(ob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ob INTERFACE cxx_std_17)

if(OB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OB_BUILD_BENCH)
    add_subdirectory(bench)
endif()

message(STATUS "orderbook ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests      : ${OB_BUILD_TESTS}")
message(STATUS "  benchmarks : ${OB_BUILD_BENCH}")
message(STATUS "  sanitizers : ${OB_SANITIZE}")
message(STATUS "  native arch: ${OB_NATIVE}")
