#include "orderbook/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace orderbook {

OrderBook::OrderPool::OrderPool(std::size_t capacity) : nodes_(capacity) {
  if (capacity == 0 || capacity >= static_cast<std::size_t>(kInvalidNode)) {
    throw std::invalid_argument("invalid order-pool capacity");
  }

  free_list_.reserve(capacity);
  for (std::size_t index = capacity; index > 0; --index) {
    free_list_.push_back(static_cast<NodeIndex>(index - 1));
  }
}

std::optional<OrderBook::NodeIndex> OrderBook::OrderPool::acquire() noexcept {
  if (free_list_.empty()) {
    return std::nullopt;
  }

  const NodeIndex index = free_list_.back();
  free_list_.pop_back();
  return index;
}

void OrderBook::OrderPool::release(NodeIndex index) noexcept {
  OrderNode &node = nodes_[index];
  node = OrderNode{};
  free_list_.push_back(index);
}

OrderBook::OrderNode &
OrderBook::OrderPool::operator[](NodeIndex index) noexcept {
  return nodes_[index];
}

const OrderBook::OrderNode &
OrderBook::OrderPool::operator[](NodeIndex index) const noexcept {
  return nodes_[index];
}

std::size_t OrderBook::OrderPool::available() const noexcept {
  return free_list_.size();
}

void OrderBook::PriceLevel::append(NodeIndex index, OrderPool &pool) noexcept {
  OrderNode &node = pool[index];
  node.previous = tail;
  node.next = kInvalidNode;

  if (tail == kInvalidNode) {
    head = index;
  } else {
    pool[tail].next = index;
  }

  tail = index;
  total_quantity += node.remaining_quantity;
  ++order_count;
}

void OrderBook::PriceLevel::unlink(NodeIndex index, OrderPool &pool) noexcept {
  OrderNode &node = pool[index];

  if (node.previous == kInvalidNode) {
    head = node.next;
  } else {
    pool[node.previous].next = node.next;
  }

  if (node.next == kInvalidNode) {
    tail = node.previous;
  } else {
    pool[node.next].previous = node.previous;
  }

  total_quantity -= node.remaining_quantity;
  --order_count;
  node.previous = kInvalidNode;
  node.next = kInvalidNode;
}

OrderBook::PriceLevel &OrderBook::PriceBlock::level(std::size_t slot) noexcept {
  return levels_[slot];
}

const OrderBook::PriceLevel &
OrderBook::PriceBlock::level(std::size_t slot) const noexcept {
  return levels_[slot];
}

bool OrderBook::PriceBlock::contains(std::size_t slot) const noexcept {
  return (occupied_slots_ & (std::uint64_t{1} << slot)) != 0;
}

void OrderBook::PriceBlock::occupy(std::size_t slot) noexcept {
  occupied_slots_ |= std::uint64_t{1} << slot;
}

void OrderBook::PriceBlock::release(std::size_t slot) noexcept {
  occupied_slots_ &= ~(std::uint64_t{1} << slot);
}

bool OrderBook::PriceBlock::empty() const noexcept {
  return occupied_slots_ == 0;
}

std::size_t OrderBook::PriceBlock::lowest_occupied_slot() const noexcept {
  assert(!empty());
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::size_t>(__builtin_ctzll(occupied_slots_));
#else
  std::uint64_t bits = occupied_slots_;
  std::size_t index = 0;
  while ((bits & 1U) == 0U) {
    bits >>= 1U;
    ++index;
  }
  return index;
#endif
}

std::size_t OrderBook::PriceBlock::highest_occupied_slot() const noexcept {
  assert(!empty());
#if defined(__GNUC__) || defined(__clang__)
  return 63U - static_cast<std::size_t>(__builtin_clzll(occupied_slots_));
#else
  std::uint64_t bits = occupied_slots_;
  std::size_t index = 0;
  while (bits >>= 1U) {
    ++index;
  }
  return index;
#endif
}

OrderBook::PriceBlockTree::BlockNumber
OrderBook::PriceBlockTree::block_number_for(PriceTick price) noexcept {
  return static_cast<BlockNumber>(price) / kLevelsPerBlock;
}

std::size_t OrderBook::PriceBlockTree::slot_for(PriceTick price) noexcept {
  return static_cast<std::size_t>(price) & (kLevelsPerBlock - 1);
}

std::optional<PriceTick>
OrderBook::PriceBlockTree::best_price() const noexcept {
  if (blocks_.empty()) {
    return std::nullopt;
  }

  const auto &block_entry = priority_ == PricePriority::Highest
                                ? *blocks_.rbegin()
                                : *blocks_.begin();
  const PriceBlock &block = block_entry.second;
  const std::size_t slot = priority_ == PricePriority::Highest
                               ? block.highest_occupied_slot()
                               : block.lowest_occupied_slot();

  return static_cast<PriceTick>(block_entry.first * kLevelsPerBlock + slot);
}

OrderBook::PriceLevel *
OrderBook::PriceBlockTree::find_level(PriceTick price) noexcept {
  const auto iterator = blocks_.find(block_number_for(price));
  if (iterator == blocks_.end()) {
    return nullptr;
  }

  const std::size_t slot = slot_for(price);
  if (!iterator->second.contains(slot)) {
    return nullptr;
  }
  return &iterator->second.level(slot);
}

const OrderBook::PriceLevel *
OrderBook::PriceBlockTree::find_level(PriceTick price) const noexcept {
  const auto iterator = blocks_.find(block_number_for(price));
  if (iterator == blocks_.end()) {
    return nullptr;
  }

  const std::size_t slot = slot_for(price);
  if (!iterator->second.contains(slot)) {
    return nullptr;
  }
  return &iterator->second.level(slot);
}

OrderBook::PriceLevel &OrderBook::PriceBlockTree::operator[](PriceTick price) {
  PriceBlock &block = blocks_[block_number_for(price)];
  const std::size_t slot = slot_for(price);
  block.occupy(slot);
  return block.level(slot);
}

void OrderBook::PriceBlockTree::erase_empty_level(PriceTick price) {
  auto iterator = blocks_.find(block_number_for(price));
  if (iterator == blocks_.end()) {
    return;
  }

  PriceBlock &block = iterator->second;
  const std::size_t slot = slot_for(price);
  PriceLevel &level = block.level(slot);
  if (level.order_count != 0) {
    return;
  }

  block.release(slot);
  if (block.empty()) {
    blocks_.erase(iterator);
  }
}

// Implementation for prewarming blocks
void OrderBook::PriceBlockTree::prewarm_blocks() {
  auto hint = blocks_.end();

  for (BlockNumber block = 0; block < prewarmed_blocks_; ++block) {
    hint = blocks_.try_emplace(hint, block);
  }

  // Destroy the temporary tree while retaining node storage in pool_.
  blocks_.clear();
}

OrderBook::OrderBook(std::size_t maximum_active_orders)
    : pool_(maximum_active_orders) {
  order_index_.reserve(maximum_active_orders);
}

bool OrderBook::can_match_at_price(Side incoming_side, PriceTick incoming_limit,
                                   PriceTick resting_price) noexcept {
  return incoming_side == Side::Buy ? resting_price <= incoming_limit
                                    : resting_price >= incoming_limit;
}

OrderBook::PriceBlockTree &OrderBook::prices_for(Side side_value) noexcept {
  return side_value == Side::Buy ? buy_prices_ : sell_prices_;
}

const OrderBook::PriceBlockTree &
OrderBook::prices_for(Side side_value) const noexcept {
  return side_value == Side::Buy ? buy_prices_ : sell_prices_;
}

OrderBook::PriceBlockTree &
OrderBook::opposite_prices_for(Side side_value) noexcept {
  return side_value == Side::Buy ? sell_prices_ : buy_prices_;
}

void OrderBook::remove_filled_resting_order(PriceBlockTree &opposite_prices,
                                            PriceTick match_price,
                                            PriceLevel &level,
                                            NodeIndex resting_index) {
  const OrderId resting_order_id = pool_[resting_index].order_id;
  level.unlink(resting_index, pool_);
  order_index_.erase(resting_order_id);
  pool_.release(resting_index);
  opposite_prices.erase_empty_level(match_price);
}

SubmitResult OrderBook::submit(const AddOrder &order,
                               TradeExecutionCallback callback,
                               void *callback_context) {
  if (order.order_id == 0 || order.price <= 0 || order.quantity <= 0) {
    return {false, RejectReason::InvalidOrder, 0, 0};
  }
  if (order_index_.find(order.order_id) != order_index_.end()) {
    return {false, RejectReason::DuplicateOrderId, 0, 0};
  }

  const std::optional<NodeIndex> acquired = pool_.acquire();
  if (!acquired.has_value()) {
    return {false, RejectReason::OrderCapacityExceeded, 0, 0};
  }

  const NodeIndex incoming_index = *acquired;
  OrderNode &incoming_order = pool_[incoming_index];
  incoming_order.order_id = order.order_id;
  incoming_order.price = order.price;
  incoming_order.remaining_quantity = order.quantity;
  incoming_order.side = order.side;
  incoming_order.active = true;

  Quantity executed_quantity = 0;
  PriceBlockTree &opposite_prices = opposite_prices_for(order.side);

  while (incoming_order.remaining_quantity > 0) {
    const std::optional<PriceTick> match_price = opposite_prices.best_price();
    if (!match_price.has_value() ||
        !can_match_at_price(order.side, order.price, *match_price)) {
      break;
    }

    PriceLevel *level = opposite_prices.find_level(*match_price);
    while (incoming_order.remaining_quantity > 0 &&
           level->head != kInvalidNode) {
      const NodeIndex resting_index = level->head;
      OrderNode &resting_order = pool_[resting_index];
      const Quantity traded = std::min(incoming_order.remaining_quantity,
                                       resting_order.remaining_quantity);

      resting_order.remaining_quantity -= traded;
      incoming_order.remaining_quantity -= traded;
      level->total_quantity -= traded;
      executed_quantity += traded;

      if (callback != nullptr) {
        callback(TradeExecution{resting_order.order_id, incoming_order.order_id,
                                resting_order.price, traded},
                 callback_context);
      }

      if (resting_order.remaining_quantity == 0) {
        remove_filled_resting_order(opposite_prices, *match_price, *level,
                                    resting_index);
        if (incoming_order.remaining_quantity > 0) {
          level = opposite_prices.find_level(*match_price);
          if (level == nullptr) {
            break;
          }
        }
      }
    }
  }

  if (incoming_order.remaining_quantity == 0) {
    pool_.release(incoming_index);
    return {true, RejectReason::None, executed_quantity, 0};
  }

  PriceBlockTree &resting_prices = prices_for(order.side);
  PriceLevel &resting_level = resting_prices[order.price];
  resting_level.append(incoming_index, pool_);
  order_index_.emplace(order.order_id, incoming_index);

  return {true, RejectReason::None, executed_quantity,
          incoming_order.remaining_quantity};
}

CancelResult OrderBook::cancel(OrderId order_id) {
  const auto order_iterator = order_index_.find(order_id);
  if (order_iterator == order_index_.end()) {
    return CancelResult::UnknownOrder;
  }

  const NodeIndex index = order_iterator->second;
  OrderNode &node = pool_[index];
  PriceBlockTree &owning_prices = prices_for(node.side);
  PriceLevel *level = owning_prices.find_level(node.price);

  const PriceTick price = node.price;
  level->unlink(index, pool_);
  order_index_.erase(order_iterator);
  pool_.release(index);
  owning_prices.erase_empty_level(price);
  return CancelResult::Canceled;
}

std::optional<PriceTick> OrderBook::best_bid() const noexcept {
  return buy_prices_.best_price();
}

std::optional<PriceTick> OrderBook::best_ask() const noexcept {
  return sell_prices_.best_price();
}

std::optional<Quantity>
OrderBook::remaining_quantity(OrderId order_id) const noexcept {
  const auto iterator = order_index_.find(order_id);
  if (iterator == order_index_.end()) {
    return std::nullopt;
  }
  return pool_[iterator->second].remaining_quantity;
}

Quantity OrderBook::level_quantity(Side side_value,
                                   PriceTick price) const noexcept {
  const PriceLevel *level = prices_for(side_value).find_level(price);
  return level == nullptr ? 0 : level->total_quantity;
}

std::size_t OrderBook::active_order_count() const noexcept {
  return order_index_.size();
}

std::size_t OrderBook::available_order_slots() const noexcept {
  return pool_.available();
}

} // namespace orderbook
