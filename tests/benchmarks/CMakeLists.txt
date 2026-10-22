target_sources(orderbook_tests PRIVATE
    fixed_rate_arrival_schedule_test.cpp
    small_book_workload_test.cpp
)
target_include_directories(orderbook_tests PRIVATE ${PROJECT_SOURCE_DIR}/benchmarks)
