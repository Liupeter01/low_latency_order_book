find_package(Threads REQUIRED)

set(ORDERBOOK_BENCHMARK_SOURCE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set(ORDERBOOK_BENCHMARK_RUNTIME_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

function(configure_orderbook_benchmark target)
    target_link_libraries(${target} PRIVATE benchmark::benchmark_main Threads::Threads)
    target_include_directories(${target} PRIVATE
        ${ORDERBOOK_BENCHMARK_SOURCE_DIRECTORY}
    )
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
    )
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${ORDERBOOK_BENCHMARK_RUNTIME_DIRECTORY}"
    )
endfunction()

add_subdirectory(channel)
add_subdirectory(order_book)
add_subdirectory(engine)
