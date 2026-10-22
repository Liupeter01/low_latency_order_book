#include "orderbook/order_book.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace orderbook::test {
namespace {

TEST(MatchingRules, UsesBestPriceBeforeArrivalTime) {
    OrderBook book(32);
    std::vector<TradeExecution> trades;

    ASSERT_TRUE(book.submit({1, Side::Sell, 101, 5}).accepted);
    ASSERT_TRUE(book.submit({2, Side::Sell, 100, 3}).accepted);
    ASSERT_TRUE(book.submit({3, Side::Sell, 100, 4}).accepted);

    const SubmitResult result = book.submit(
        {10, Side::Buy, 101, 9}, collect_trade, &trades);

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.executed_quantity, 9);
    EXPECT_EQ(result.resting_quantity, 0);
    ASSERT_EQ(trades.size(), 3U);

    EXPECT_EQ(trades[0].maker_order_id, 2U);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 3);
    EXPECT_EQ(trades[1].maker_order_id, 3U);
    EXPECT_EQ(trades[1].price, 100);
    EXPECT_EQ(trades[1].quantity, 4);
    EXPECT_EQ(trades[2].maker_order_id, 1U);
    EXPECT_EQ(trades[2].price, 101);
    EXPECT_EQ(trades[2].quantity, 2);
    ASSERT_TRUE(book.remaining_quantity(1).has_value());
    EXPECT_EQ(*book.remaining_quantity(1), 3);
}

TEST(MatchingRules, PartialFillRestsAtOriginalLimitPrice) {
    OrderBook book(32);
    std::vector<TradeExecution> trades;

    ASSERT_TRUE(book.submit({1, Side::Sell, 29'700, 10'0000}).accepted);
    ASSERT_TRUE(book.submit({2, Side::Sell, 29'800, 38'0000}).accepted);

    const SubmitResult result = book.submit(
        {100, Side::Buy, 30'000, 100'0000}, collect_trade, &trades);

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.executed_quantity, 48'0000);
    EXPECT_EQ(result.resting_quantity, 52'0000);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].price, 29'700);
    EXPECT_EQ(trades[0].quantity, 10'0000);
    EXPECT_EQ(trades[1].price, 29'800);
    EXPECT_EQ(trades[1].quantity, 38'0000);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 30'000);
    ASSERT_TRUE(book.remaining_quantity(100).has_value());
    EXPECT_EQ(*book.remaining_quantity(100), 52'0000);
    EXPECT_EQ(book.level_quantity(Side::Buy, 30'000), 52'0000);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(MatchingRules, NonCrossingOrderRestsWithoutTrade) {
    OrderBook book(8);
    std::vector<TradeExecution> trades;

    ASSERT_TRUE(book.submit({1, Side::Sell, 101, 5}).accepted);
    const SubmitResult result = book.submit(
        {2, Side::Buy, 100, 5}, collect_trade, &trades);

    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.executed_quantity, 0);
    EXPECT_EQ(result.resting_quantity, 5);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
}

}  // namespace
}  // namespace orderbook::test
