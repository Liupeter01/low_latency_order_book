#pragma once

#include "orderbook/engine.hpp"

#include <cstdint>

namespace orderbook::benchmarking {

// A small deterministic cycle for the complete engine benchmark:
//
//   0. sell 100 @ trade_price       -> rests
//   1. buy  100 @ trade_price - 1   -> rests one tick below the sell
//   2. buy  100 @ trade_price       -> matches the sell
//   3. cancel the lower buy         -> book is empty again
class SmallBookWorkload {
public:
  static constexpr std::uint64_t kRequestsPerCycle = 4;
  static constexpr std::uint64_t kEventsPerCycle = 5;
  static constexpr Quantity kQuantity = 100;
  static constexpr PriceTick kOneTick = 1;

  OrderRequest request(std::uint64_t index) const noexcept {
    const Sequence sequence = index + 1;
    const PriceTick price = trade_price(index);

    switch (index % kRequestsPerCycle) {
    case 0: // Add the sell that will be matched later in this cycle.
      return OrderRequest::add(sequence, sequence, Side::Sell, price,
                               kQuantity);
    case 1: // Rest a buy exactly one tick below that sell.
      return OrderRequest::add(sequence, sequence, Side::Buy, price - kOneTick,
                               kQuantity);
    case 2: // Cross the sell at its limit price.
      return OrderRequest::add(sequence, sequence, Side::Buy, price, kQuantity);
    default: // Cancel the lower buy created two requests earlier.
      return OrderRequest::cancel(sequence, sequence - 2);
    }
  }

  bool validate_trade(const OrderEvent &event,
                      std::uint64_t index) const noexcept {
    if (!is_matching_buy(index)) {
      return false;
    }
    const OrderRequest matching_buy = request(index);
    return event.type == OrderEventType::Trade &&
           event.sequence == matching_buy.sequence &&
           event.maker_order_id == matching_buy.order_id - 2 &&
           event.taker_order_id == matching_buy.order_id &&
           event.price == matching_buy.price && event.quantity == kQuantity;
  }

  bool validate_completion(const OrderEvent &event, std::uint64_t index,
                           std::uint64_t trade_count) const noexcept {
    const OrderRequest current = request(index);
    const bool cancel = is_cancel(index);
    const bool matched = is_matching_buy(index);

    return event.sequence == current.sequence &&
           event.order_id == current.order_id &&
           event.type == (cancel ? OrderEventType::OrderCanceled
                                 : OrderEventType::OrderAccepted) &&
           event.reject_reason == RejectReason::None &&
           event.remaining_quantity == (cancel || matched ? 0 : kQuantity) &&
           trade_count == (matched ? 1U : 0U);
  }

private:
  static bool is_matching_buy(std::uint64_t index) noexcept {
    return index % kRequestsPerCycle == 2;
  }

  static bool is_cancel(std::uint64_t index) noexcept {
    return index % kRequestsPerCycle == 3;
  }

  static PriceTick trade_price(std::uint64_t index) noexcept {
    const std::uint64_t cycle = index / kRequestsPerCycle;
    return 10'000 + static_cast<PriceTick>(cycle % 64);
  }
};

} // namespace orderbook::benchmarking
