#pragma once

#include "orderbook/order_book.hpp"
#include "orderbook/spsc_channel.hpp"
#include "orderbook/types.hpp"
#include "spinbackoff/spin_backoff.hpp"

#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace orderbook {

using Sequence = std::uint64_t;

enum class OrderRequestType : std::uint8_t {
  Add,
  Cancel,
  Shutdown,
};

// Flat, trivially-copyable messages are convenient for bounded ring buffers.
struct OrderRequest {
  OrderRequestType type{OrderRequestType::Add};
  Side side{Side::Buy};
  Sequence sequence{};
  OrderId order_id{};
  PriceTick price{};
  Quantity quantity{};

  static OrderRequest add(Sequence sequence_value, OrderId id, Side order_side,
                          PriceTick order_price,
                          Quantity order_quantity) noexcept {
    return OrderRequest{OrderRequestType::Add, order_side,
                        sequence_value,        id,
                        order_price,           order_quantity};
  }

  static OrderRequest cancel(Sequence sequence_value, OrderId id) noexcept {
    OrderRequest request;
    request.type = OrderRequestType::Cancel;
    request.sequence = sequence_value;
    request.order_id = id;
    return request;
  }

  static OrderRequest shutdown(Sequence sequence_value) noexcept {
    OrderRequest request;
    request.type = OrderRequestType::Shutdown;
    request.sequence = sequence_value;
    return request;
  }
};

enum class OrderEventType : std::uint8_t {
  Trade,
  OrderAccepted,
  OrderRejected,
  OrderCanceled,
  CancelRejected,
  EngineStopped,
};

struct OrderEvent {
  OrderEventType type{OrderEventType::OrderAccepted};
  RejectReason reject_reason{RejectReason::None};
  Sequence sequence{};
  OrderId order_id{};
  OrderId maker_order_id{};
  OrderId taker_order_id{};
  PriceTick price{};
  Quantity quantity{};
  Quantity remaining_quantity{};
};

template <std::size_t Capacity>
using RequestChannel = SpscChannel<OrderRequest, Capacity>;

template <std::size_t Capacity>
using EventChannel = SpscChannel<OrderEvent, Capacity>;

// Front-end API. This type owns only the request-writer endpoint and cannot
// inspect the book or consume order events.
template <std::size_t Capacity> class OrderRequestWriter {
public:
  using Writer = typename RequestChannel<Capacity>::Writer;

  explicit OrderRequestWriter(Writer writer) noexcept
      : writer_(std::move(writer)) {}

  bool try_submit(Sequence sequence, const AddOrder &order) noexcept {
    return writer_.try_write(OrderRequest::add(
        sequence, order.order_id, order.side, order.price, order.quantity));
  }

  bool try_cancel(Sequence sequence, OrderId order_id) noexcept {
    return writer_.try_write(OrderRequest::cancel(sequence, order_id));
  }

  // The sole producer calls this only after it has stopped sending orders.
  bool try_request_stop(Sequence sequence) noexcept {
    return writer_.try_write(OrderRequest::shutdown(sequence));
  }

private:
  Writer writer_;
};

// Back-end API. This type owns only the event-reader endpoint and cannot submit
// or cancel orders.
template <std::size_t Capacity> class OrderEventReader {
public:
  using Reader = typename EventChannel<Capacity>::Reader;

  explicit OrderEventReader(Reader reader) noexcept
      : reader_(std::move(reader)) {}

  bool try_read(OrderEvent &event) noexcept { return reader_.try_read(event); }

  std::size_t approximate_size() const noexcept {
    return reader_.approximate_size();
  }

private:
  Reader reader_;
};

// The matching engine owns the opposite endpoints. It reads requests and
// writes events, but does not expose either transport operation publicly.
template <std::size_t RequestCapacity, std::size_t EventCapacity>
class MatchingEngine {
public:
  using RequestReader = typename RequestChannel<RequestCapacity>::Reader;
  using EventWriter = typename EventChannel<EventCapacity>::Writer;

  MatchingEngine(RequestReader requests, EventWriter events,
                 std::size_t maximum_active_orders)
      : requests_(std::move(requests)), events_(std::move(events)),
        book_(maximum_active_orders) {}

  void run() {
    TradeContext trade_context{this, 0};

    spinbackoff::SpinBackoff backoff;
    for (;;) {
      OrderRequest request;
      if (!requests_.try_read(request)) {
        backoff.wait();
        continue;
      }

      backoff.reset();

      trade_context.sequence = request.sequence;
      if (request.type == OrderRequestType::Shutdown) {
        OrderEvent event;
        event.type = OrderEventType::EngineStopped;
        event.sequence = request.sequence;
        write_event(event);
        return;
      }

      if (request.type == OrderRequestType::Cancel) {
        const CancelResult result = book_.cancel(request.order_id);
        OrderEvent event;
        event.type = result == CancelResult::Canceled
                         ? OrderEventType::OrderCanceled
                         : OrderEventType::CancelRejected;
        event.sequence = request.sequence;
        event.order_id = request.order_id;
        write_event(event);
        continue;
      }

      const AddOrder order{request.order_id, request.side, request.price,
                           request.quantity};
      const SubmitResult result =
          book_.submit(order, &MatchingEngine::on_trade, &trade_context);

      OrderEvent event;
      event.type = result.accepted ? OrderEventType::OrderAccepted
                                   : OrderEventType::OrderRejected;
      event.reject_reason = result.reject_reason;
      event.sequence = request.sequence;
      event.order_id = request.order_id;
      event.remaining_quantity = result.resting_quantity;
      write_event(event);
    }
  }

private:
  struct TradeContext {
    MatchingEngine *engine;
    Sequence sequence;
  };

  void write_event(const OrderEvent &event) {
    // Events may not be dropped. A production application can replace
    // this policy with batching or a larger channel after measurement.
    spinbackoff::SpinBackoff backoff;
    while (!events_.try_write(event)) {
      backoff.wait();
    }

    //backoff.reset();
  }

  static void on_trade(const TradeExecution &trade, void *raw_context) {
    auto &context = *static_cast<TradeContext *>(raw_context);
    OrderEvent event;
    event.type = OrderEventType::Trade;
    event.sequence = context.sequence;
    event.maker_order_id = trade.maker_order_id;
    event.taker_order_id = trade.taker_order_id;
    event.price = trade.price;
    event.quantity = trade.quantity;
    context.engine->write_event(event);
  }

  RequestReader requests_;
  EventWriter events_;
  OrderBook book_;
};

} // namespace orderbook
