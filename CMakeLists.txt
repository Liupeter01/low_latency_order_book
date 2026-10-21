cmake_minimum_required(VERSION 3.16)
option(ORDERBOOK_BUILD_TESTS "Build GoogleTest executables" ON)
option(ORDERBOOK_BUILD_BENCHMARKS "Build Google Benchmark executables" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(low_latency_order_book LANGUAGES CXX)

target_include_directories(orderbook PUBLIC include)
#add_library(orderbook src/order_book.cpp)
#target_compile_options(orderbook PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)

#add_executable(orderbook_demo examples/demo.cpp)
#target_link_libraries(orderbook_demo PRIVATE orderbook)

if(ORDERBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ORDERBOOK_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG QUIET)
    if(NOT TARGET benchmark::benchmark)
        include(FetchContent)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.9.4
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(googlebenchmark)
    endif()
    add_subdirectory(benchmarks)
endif()
