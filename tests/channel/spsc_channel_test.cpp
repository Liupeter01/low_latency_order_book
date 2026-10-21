#include "orderbook/spsc_channel.hpp"
#include "orderbook/types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

namespace orderbook::test {
namespace {

TEST(SpscChannel, PreservesFifoOrderAndReportsFull) {
    SpscChannel<OrderRequest, 4> channel;
    auto writer = channel.make_writer();
    auto reader = channel.make_reader();

    EXPECT_TRUE(writer.try_write(OrderRequest::cancel(1, 11)));
    EXPECT_TRUE(writer.try_write(OrderRequest::cancel(2, 22)));
    EXPECT_TRUE(writer.try_write(OrderRequest::cancel(3, 33)));
    EXPECT_TRUE(writer.try_write(OrderRequest::cancel(4, 44)));
    EXPECT_FALSE(writer.try_write(OrderRequest::cancel(5, 55)));

    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        OrderRequest request;
        ASSERT_TRUE(reader.try_read(request));
        EXPECT_EQ(request.sequence, sequence);
        EXPECT_EQ(request.order_id, sequence * 11);
    }

    OrderRequest request;
    EXPECT_FALSE(reader.try_read(request));
}

TEST(SpscChannel, TransfersMessagesBetweenTwoThreads) {
    constexpr std::uint64_t kMessageCount = 100'000;
    SpscChannel<OrderRequest, 1024> channel;
    auto writer = channel.make_writer();
    auto reader = channel.make_reader();

    std::thread producer([writer = std::move(writer)]() mutable {
        for (std::uint64_t sequence = 1; sequence <= kMessageCount; ++sequence) {
            const OrderRequest request = OrderRequest::cancel(sequence, sequence);
            while (!writer.try_write(request)) {
                std::this_thread::yield();
            }
        }
    });

    for (std::uint64_t sequence = 1; sequence <= kMessageCount; ++sequence) {
        OrderRequest request;
        while (!reader.try_read(request)) {
            std::this_thread::yield();
        }
        ASSERT_EQ(request.sequence, sequence);
        ASSERT_EQ(request.order_id, sequence);
    }

    producer.join();
}

}  // namespace
}  // namespace orderbook::test
