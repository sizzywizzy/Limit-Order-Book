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
#   ob_test_basic        unit tests            — phase 1
#   ob_test_properties   randomised invariants — phase 1
#   ob_test_differential naive vs fast         — phase 2
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
