#include "orderbook/order_book.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace orderbook::test {
namespace {

TEST(Cancellation, RemovesAnOrderFromTheMiddleOfOnePriceLevel) {
    OrderBook book(32);
    std::vector<TradeExecution> trades;

    ASSERT_TRUE(book.submit({1, Side::Sell, 100, 2}).accepted);
    ASSERT_TRUE(book.submit({2, Side::Sell, 100, 3}).accepted);
    ASSERT_TRUE(book.submit({3, Side::Sell, 100, 5}).accepted);

    EXPECT_EQ(book.cancel(2), CancelResult::Canceled);
    EXPECT_EQ(book.cancel(2), CancelResult::UnknownOrder);
    EXPECT_EQ(book.level_quantity(Side::Sell, 100), 7);

    ASSERT_TRUE(book.submit({9, Side::Buy, 100, 7},
                            collect_trade, &trades).accepted);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].maker_order_id, 1U);
    EXPECT_EQ(trades[1].maker_order_id, 3U);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(Validation, RejectsInvalidAndDuplicateOrders) {
    OrderBook book(4);

    EXPECT_EQ(book.submit({0, Side::Buy, 100, 1}).reject_reason,
              RejectReason::InvalidOrder);
    EXPECT_EQ(book.submit({1, Side::Buy, 0, 1}).reject_reason,
              RejectReason::InvalidOrder);
    EXPECT_EQ(book.submit({1, Side::Buy, 100, 0}).reject_reason,
              RejectReason::InvalidOrder);

    ASSERT_TRUE(book.submit({1, Side::Buy, 100, 1}).accepted);
    EXPECT_EQ(book.submit({1, Side::Buy, 99, 1}).reject_reason,
              RejectReason::DuplicateOrderId);
}

TEST(OrderPool, ReusesReleasedCapacity) {
    OrderBook book(1);

    ASSERT_TRUE(book.submit({1, Side::Buy, 100, 1}).accepted);
    EXPECT_EQ(book.available_order_slots(), 0U);
    EXPECT_EQ(book.submit({2, Side::Buy, 99, 1}).reject_reason,
              RejectReason::OrderCapacityExceeded);

    ASSERT_EQ(book.cancel(1), CancelResult::Canceled);
    EXPECT_EQ(book.available_order_slots(), 1U);
    EXPECT_TRUE(book.submit({2, Side::Buy, 99, 1}).accepted);
}

}  // namespace
}  // namespace orderbook::test
