#pragma once

#include "orderbook/order_book.hpp"

#include <vector>

namespace orderbook::test {

inline void collect_trade(const TradeExecution& trade, void* context) {
    static_cast<std::vector<TradeExecution>*>(context)->push_back(trade);
}

}  // namespace orderbook::test
