add_executable(ob_bench
    bench_main.cpp
    generator.cpp)

target_link_libraries(ob_bench PRIVATE ob ob_flags)

# A benchmark built without optimisation measures the debug build, not the
# engine. Say so loudly rather than letting a bad number get recorded.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING
        "ob_bench is being built in Debug. Any latency figure from this build "
        "is meaningless — configure with -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=ON.")
endif()
