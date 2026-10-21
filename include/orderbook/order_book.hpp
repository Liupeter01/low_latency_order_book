#pragma once

#include "orderbook/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orderbook {

// Represents an order to be added to the order book.
struct AddOrder {
  OrderId order_id{};
  Side side{};
  PriceTick price{};
  Quantity quantity{};
};

// Represents a trade execution between a maker and a taker order.
struct TradeExecution {
  OrderId maker_order_id{};
  OrderId taker_order_id{};
  PriceTick price{};
  Quantity quantity{};
};

enum class RejectReason : std::uint8_t {
  None,
  InvalidOrder,
  DuplicateOrderId,
  OrderCapacityExceeded,
};

struct SubmitResult {
  bool accepted{};
  RejectReason reject_reason{RejectReason::None};
  Quantity executed_quantity{};
  Quantity resting_quantity{};
};

enum class CancelResult : std::uint8_t {
  Canceled,
  UnknownOrder,
};

using TradeExecutionCallback = void (*)(const TradeExecution &,
                                        void *context);

class OrderBook {
public:
  static constexpr std::size_t kLevelsPerBlock = 64;

  explicit OrderBook(std::size_t maximum_active_orders);

  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;

  SubmitResult submit(const AddOrder &order,
                      TradeExecutionCallback callback = nullptr,
                      void *callback_context = nullptr);
  CancelResult cancel(OrderId order_id);

  std::optional<PriceTick> best_bid() const noexcept;
  std::optional<PriceTick> best_ask() const noexcept;
  std::optional<Quantity> remaining_quantity(OrderId order_id) const noexcept;
  Quantity level_quantity(Side side, PriceTick price) const noexcept;
  std::size_t active_order_count() const noexcept;
  std::size_t available_order_slots() const noexcept;

private:
  using NodeIndex = std::uint32_t;
  static constexpr NodeIndex kInvalidNode =
      std::numeric_limits<NodeIndex>::max();

  struct alignas(16) OrderNode {
    OrderId order_id{};
    PriceTick price{};
    Quantity remaining_quantity{};
    NodeIndex previous{kInvalidNode};
    NodeIndex next{kInvalidNode};
    Side side{Side::Buy};
    bool active{};
  };

  class OrderPool {
  public:
    explicit OrderPool(std::size_t capacity);

    std::optional<NodeIndex> acquire() noexcept;
    void release(NodeIndex index) noexcept;
    OrderNode &operator[](NodeIndex index) noexcept;
    const OrderNode &operator[](NodeIndex index) const noexcept;
    std::size_t available() const noexcept;

  private:
    std::vector<OrderNode> nodes_;
    std::vector<NodeIndex> free_list_;
  };

  // One FIFO of orders sharing the same price.
  struct PriceLevel {
    void append(NodeIndex index, OrderPool &pool) noexcept;
    void unlink(NodeIndex index, OrderPool &pool) noexcept;

    NodeIndex head{kInvalidNode};
    NodeIndex tail{kInvalidNode};
    Quantity total_quantity{};
    std::uint32_t order_count{};
  };

  // Sixty-four consecutive price levels and their occupancy bitmap.
  struct PriceBlock {
    PriceLevel &level(std::size_t slot) noexcept;
    const PriceLevel &level(std::size_t slot) const noexcept;
    bool contains(std::size_t slot) const noexcept;
    void occupy(std::size_t slot) noexcept;
    void release(std::size_t slot) noexcept;
    bool empty() const noexcept;
    std::size_t lowest_occupied_slot() const noexcept;
    std::size_t highest_occupied_slot() const noexcept;

  private:
    std::array<PriceLevel, kLevelsPerBlock> levels_{};
    std::uint64_t occupied_slots_{};
  };

  enum class PricePriority : std::uint8_t {
    Lowest,
    Highest,
  };

  // A binary-search tree of non-empty price blocks for one order side.
  class PriceBlockTree {
  public:
    explicit PriceBlockTree(PricePriority priority) : priority_(priority) {}

    std::optional<PriceTick> best_price() const noexcept;
    PriceLevel *find_level(PriceTick price) noexcept;
    const PriceLevel *find_level(PriceTick price) const noexcept;
    PriceLevel &operator[](PriceTick price);
    void erase_empty_level(PriceTick price);

  private:
    using BlockNumber = std::uint64_t;

    static BlockNumber block_number_for(PriceTick price) noexcept;
    static std::size_t slot_for(PriceTick price) noexcept;

    PricePriority priority_;
    std::map<BlockNumber, PriceBlock> blocks_;
  };

  static bool can_match_at_price(Side incoming_side,
                                 PriceTick incoming_limit,
                                 PriceTick resting_price) noexcept;

  PriceBlockTree &prices_for(Side side_value) noexcept;
  const PriceBlockTree &prices_for(Side side_value) const noexcept;
  PriceBlockTree &opposite_prices_for(Side side_value) noexcept;

  void remove_filled_resting_order(PriceBlockTree &opposite_prices,
                                   PriceTick match_price,
                                   PriceLevel &level,
                                   NodeIndex resting_index);

  PriceBlockTree buy_prices_{PricePriority::Highest};
  PriceBlockTree sell_prices_{PricePriority::Lowest};
  OrderPool pool_;
  std::unordered_map<OrderId, NodeIndex> order_index_;
};

} // namespace orderbook
