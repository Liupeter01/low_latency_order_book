#include "orderbook/engine.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace orderbook::test {
namespace {

TEST(MatchingEngine, ConnectsRequestAndEventInterfaces) {
    RequestChannel<64> requests;
    EventChannel<64> events;
    OrderRequestWriter<64> request_writer(requests.make_writer());
    OrderEventReader<64> event_reader(events.make_reader());
    MatchingEngine<64, 64> engine(requests.make_reader(),
                                  events.make_writer(), 64);

    std::thread engine_thread([&engine] { engine.run(); });

    ASSERT_TRUE(request_writer.try_submit(
        1, {1, Side::Sell, 29'700, 10'0000}));
    ASSERT_TRUE(request_writer.try_submit(
        2, {2, Side::Sell, 29'800, 38'0000}));
    ASSERT_TRUE(request_writer.try_submit(
        3, {100, Side::Buy, 30'000, 100'0000}));
    ASSERT_TRUE(request_writer.try_request_stop(4));

    std::vector<OrderEvent> received;
    for (;;) {
        OrderEvent event;
        if (!event_reader.try_read(event)) {
            std::this_thread::yield();
            continue;
        }
        received.push_back(event);
        if (event.type == OrderEventType::EngineStopped) {
            break;
        }
    }

    engine_thread.join();

    ASSERT_EQ(received.size(), 6U);
    EXPECT_EQ(received[0].type, OrderEventType::OrderAccepted);
    EXPECT_EQ(received[1].type, OrderEventType::OrderAccepted);
    EXPECT_EQ(received[2].type, OrderEventType::Trade);
    EXPECT_EQ(received[2].price, 29'700);
    EXPECT_EQ(received[2].quantity, 10'0000);
    EXPECT_EQ(received[3].type, OrderEventType::Trade);
    EXPECT_EQ(received[3].price, 29'800);
    EXPECT_EQ(received[3].quantity, 38'0000);
    EXPECT_EQ(received[4].type, OrderEventType::OrderAccepted);
    EXPECT_EQ(received[4].remaining_quantity, 52'0000);
    EXPECT_EQ(received[5].type, OrderEventType::EngineStopped);
}

}  // namespace
}  // namespace orderbook::test
