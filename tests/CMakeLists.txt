find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(orderbook_tests)

target_link_libraries(orderbook_tests PRIVATE
    orderbook
    GTest::gtest_main
)

target_include_directories(orderbook_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)

target_compile_options(orderbook_tests PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)

add_subdirectory(order_book)
add_subdirectory(channel)
#add_subdirectory(engine)

gtest_discover_tests(orderbook_tests)
