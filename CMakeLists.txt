
cmake_minimum_required(VERSION 3.23)
project(matperf)

add_library(
    matperf
    STATIC
    src/profiler.cpp
)

target_include_directories(
    matperf
    PUBLIC
    include
)

target_compile_features(
    matperf
    PRIVATE
    cxx_std_20
    )

find_package(Threads)
target_link_libraries(
    matperf
    PRIVATE
    ${CMAKE_THREAD_LIBS_INIT}
    )
