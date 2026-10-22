#include "orderbook/order_book.hpp"

#include <gtest/gtest.h>

namespace orderbook::test {
namespace {

TEST(PriceBlocks, FindsBestPriceAcrossBlockBoundaries) {
    OrderBook book(16);

    ASSERT_TRUE(book.submit({1, Side::Buy, 63, 1}).accepted);
    ASSERT_TRUE(book.submit({2, Side::Buy, 64, 1}).accepted);
    ASSERT_TRUE(book.submit({3, Side::Buy, 127, 1}).accepted);
    ASSERT_TRUE(book.submit({4, Side::Buy, 128, 1}).accepted);
    EXPECT_EQ(book.best_bid(), 128);

    ASSERT_EQ(book.cancel(4), CancelResult::Canceled);
    EXPECT_EQ(book.best_bid(), 127);
    ASSERT_EQ(book.cancel(3), CancelResult::Canceled);
    EXPECT_EQ(book.best_bid(), 64);
}

TEST(PriceBlocks, RemovesEmptyBlocksOnBothSides) {
    OrderBook book(16);

    ASSERT_TRUE(book.submit({1, Side::Buy, 64, 2}).accepted);
    ASSERT_TRUE(book.submit({2, Side::Sell, 192, 3}).accepted);
    ASSERT_EQ(book.cancel(1), CancelResult::Canceled);
    ASSERT_EQ(book.cancel(2), CancelResult::Canceled);

    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.active_order_count(), 0U);
}

TEST(PriceBlocks, UsesHighestBidAndLowestAskInsideOneBlock) {
    OrderBook book(16);

    ASSERT_TRUE(book.submit({1, Side::Buy, 65, 1}).accepted);
    ASSERT_TRUE(book.submit({2, Side::Buy, 95, 1}).accepted);
    ASSERT_TRUE(book.submit({3, Side::Sell, 120, 1}).accepted);
    ASSERT_TRUE(book.submit({4, Side::Sell, 110, 1}).accepted);

    EXPECT_EQ(book.best_bid(), 95);
    EXPECT_EQ(book.best_ask(), 110);
}

}  // namespace
}  // namespace orderbook::test
