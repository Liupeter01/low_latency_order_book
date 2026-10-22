add_executable(orderbook_benchmarks
    order_book_throughput_benchmark.cpp
    spsc_handoff_benchmark.cpp
)

target_link_libraries(orderbook_benchmarks PRIVATE
    orderbook
    benchmark::benchmark_main
)

target_compile_options(orderbook_benchmarks PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)
