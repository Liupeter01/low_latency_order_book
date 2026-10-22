#include "common/benchmark_clock.hpp"
#include "engine/small_book_workload.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace orderbook::benchmarking {
namespace {

TEST(SmallBookWorkload, BuildsOneReadableFourRequestCycle) {
  const SmallBookWorkload workload;
  const OrderRequest sell = workload.request(0);
  const OrderRequest lower_buy = workload.request(1);
  const OrderRequest matching_buy = workload.request(2);
  const OrderRequest cancel = workload.request(3);

  EXPECT_EQ(sell.type, OrderRequestType::Add);
  EXPECT_EQ(sell.side, Side::Sell);
  EXPECT_EQ(sell.price, 10'000);
  EXPECT_EQ(lower_buy.side, Side::Buy);
  EXPECT_EQ(lower_buy.price, sell.price - SmallBookWorkload::kOneTick);
  EXPECT_EQ(matching_buy.side, Side::Buy);
  EXPECT_EQ(matching_buy.price, sell.price);
  EXPECT_EQ(cancel.type, OrderRequestType::Cancel);
  EXPECT_EQ(cancel.order_id, lower_buy.order_id);
}

TEST(SmallBookWorkload, ValidatesAndDrainsThroughTinyChannels) {
  constexpr std::uint64_t count = 4096;
  const SmallBookWorkload workload;
  RequestChannel<2> requests;
  EventChannel<2> events;
  // Two orders rest before the matching buy arrives. The taker needs a third
  // temporary pool slot before it consumes and releases the resting sell.
  MatchingEngine<2, 2> engine(requests.make_reader(), events.make_writer(), 3);
  auto writer = requests.make_writer();
  auto reader = events.make_reader();

  std::thread engine_thread([&] { engine.run(); });
  std::thread producer([&] {
    for (std::uint64_t index = 0; index < count; ++index) {
      const auto request = workload.request(index);
      while (!writer.try_write(request)) {
        cpu_relax();
      }
    }
    while (!writer.try_write(OrderRequest::shutdown(count + 1))) {
      cpu_relax();
    }
  });

  bool valid = true;
  std::uint64_t request_index = 0;
  std::uint64_t preceding_trades = 0;
  std::uint64_t event_count = 0;
  for (;;) {
    OrderEvent event;
    if (!reader.try_read(event)) {
      cpu_relax();
      continue;
    }
    if (event.type == OrderEventType::EngineStopped) {
      valid = valid && event.sequence == count + 1;
      break;
    }
    ++event_count;
    if (event.type == OrderEventType::Trade) {
      valid = workload.validate_trade(event, request_index) && valid;
      ++preceding_trades;
    } else {
      valid = workload.validate_completion(event, request_index,
                                           preceding_trades) && valid;
      preceding_trades = 0;
      ++request_index;
    }
  }
  producer.join();
  engine_thread.join();

  EXPECT_TRUE(valid);
  EXPECT_EQ(request_index, count);
  EXPECT_EQ(preceding_trades, 0U);
  EXPECT_EQ(event_count,
            count * SmallBookWorkload::kEventsPerCycle /
                SmallBookWorkload::kRequestsPerCycle);
}

TEST(SmallBookWorkload, DetectsMissingTradeAndWrongAcknowledgment) {
  const SmallBookWorkload workload;
  constexpr std::uint64_t matching_buy_index = 2;
  const auto request = workload.request(matching_buy_index);
  OrderEvent event;
  event.type = OrderEventType::OrderAccepted;
  event.sequence = request.sequence;
  event.order_id = request.order_id;

  EXPECT_FALSE(
      workload.validate_completion(event, matching_buy_index, 0));
  EXPECT_TRUE(workload.validate_completion(event, matching_buy_index, 1));
  event.remaining_quantity = SmallBookWorkload::kQuantity;
  EXPECT_FALSE(
      workload.validate_completion(event, matching_buy_index, 1));
  event.remaining_quantity = 0;
  event.sequence = 1000;
  EXPECT_FALSE(
      workload.validate_completion(event, matching_buy_index, 1));
}

} // namespace
} // namespace orderbook::benchmarking
