# This target intentionally depends only on the channel header, not the engine
# or order-book implementation.
add_executable(spsc_benchmarks spsc_handoff_benchmark.cpp)
target_include_directories(spsc_benchmarks PRIVATE ${PROJECT_SOURCE_DIR}/include)
configure_orderbook_benchmark(spsc_benchmarks)
