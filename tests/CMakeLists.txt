# Prefer a system Catch2 so an offline build works; fall back to fetching it.
find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found locally — fetching v3.5.2")
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    # A fetched Catch2 does not put its CMake helpers on the module path the
    # way an installed one does.
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

include(Catch)

# One executable per test layer, so a failure names its layer and so the slow
# layers can be run on their own. See README.md, "Testing".
#
#   ob_test_basic        unit tests + component tests
#   ob_test_properties   randomised invariants, replay, zero-alloc proof
#   ob_test_differential naive vs fast, identical event streams
set(OB_TEST_TARGETS
    basic
    properties
    differential)

foreach(layer IN LISTS OB_TEST_TARGETS)
    add_executable(ob_test_${layer} test_${layer}.cpp)
    target_link_libraries(ob_test_${layer}
        PRIVATE ob ob_flags Catch2::Catch2WithMain)
    catch_discover_tests(ob_test_${layer})
endforeach()

# The same harness and scenarios with no framework dependency at all — for
# environments without CMake or Catch2 (it builds with a bare compiler:
# g++ -std=c++23 -O2 -I include tests/standalone_runner.cpp). Registered in
# CTest so it cannot bitrot; arguments scale it up to the full definition-of-
# done run (see the file header).
add_executable(ob_test_standalone standalone_runner.cpp)
target_link_libraries(ob_test_standalone PRIVATE ob ob_flags)
add_test(NAME standalone_runner COMMAND ob_test_standalone 200 200 60000)
