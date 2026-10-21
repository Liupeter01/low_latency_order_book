#pragma once

#include <cstdint>

namespace orderbook {

using OrderId = std::uint64_t;
using PriceTick = std::int64_t;
using Quantity = std::int64_t;

enum class Side : std::uint8_t {
  Buy,
  Sell,
};

} // namespace orderbook
