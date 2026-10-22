#include "cpu_affinity.hpp"
#include "latency_statistics.hpp"

#include "orderbook/order_book.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace orderbook::benchmarking {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kWarmupOperations = 10'000;
constexpr std::uint64_t kOperationsPerBatch = 16;

std::int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

template <typename Operation>
bool measure_throughput_batches(benchmark::State &state,
                                std::uint64_t total_operations,
                                Operation &&operation) {
  if (total_operations <= kWarmupOperations) {
    state.SkipWithError("operation count must exceed warmup count");
    return false;
  }

  const std::uint64_t measured_operations =
      total_operations - kWarmupOperations;
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(
      (measured_operations + kOperationsPerBatch - 1) / kOperationsPerBatch));
  bool valid = true;

  for (std::uint64_t index = 0; index < kWarmupOperations; ++index) {
    valid = operation(index) && valid;
  }

  std::int64_t measured_ns = 0;
  for (std::uint64_t begin_index = kWarmupOperations;
       begin_index < total_operations;) {
    const std::uint64_t batch_size =
        std::min(kOperationsPerBatch, total_operations - begin_index);
    const std::int64_t started_ns = now_ns();

    for (std::uint64_t offset = 0; offset < batch_size; ++offset) {
      valid = operation(begin_index + offset) && valid;
    }

    const std::int64_t completed_ns = now_ns();
    if (completed_ns < started_ns) {
      valid = false;
    } else {
      const std::int64_t batch_ns = completed_ns - started_ns;
      measured_ns += batch_ns;
      samples.push_back(batch_ns / static_cast<std::int64_t>(batch_size));
    }
    begin_index += batch_size;
  }

  state.SetIterationTime(static_cast<double>(measured_ns) / 1'000'000'000.0);

  if (!valid || samples.empty()) {
    state.SkipWithError("order-book throughput benchmark validation failed");
    return false;
  }

  state.counters["operations_per_batch"] = kOperationsPerBatch;
  report_batch_averages(state, std::move(samples));
  return true;
}

CpuBindingResult apply_engine_cpu_binding() noexcept {
  return bind_current_thread_from_environment("ORDERBOOK_BENCHMARK_ENGINE_CPU");
}

void finish_throughput_benchmark(benchmark::State &state,
                                 std::uint64_t total_operations,
                                 const char *operation,
                                 const CpuBindingResult &binding) {
  state.SetItemsProcessed(static_cast<std::int64_t>(
      state.iterations() * (total_operations - kWarmupOperations)));
  state.SetLabel(std::to_string(total_operations) + " " + operation +
                 " operations; first " + std::to_string(kWarmupOperations) +
                 " warmup; service time is the per-operation average of " +
                 std::to_string(kOperationsPerBatch) + "-operation batches; " +
                 binding_description("engine", binding));
}

void count_trade(const TradeExecution &, void *context) {
  ++*static_cast<std::uint64_t *>(context);
}

void BM_OrderBook_RestingAdd_Throughput(benchmark::State &state) {
  static_assert(Clock::is_steady,
                "throughput timing requires a monotonic steady clock");
  const auto total = static_cast<std::uint64_t>(state.range(0));
  const CpuBindingResult binding = apply_engine_cpu_binding();

  for (auto _ : state) {
    (void)_;
    OrderBook book(static_cast<std::size_t>(total + 1));
    measure_throughput_batches(state, total, [&](std::uint64_t index) {
      const SubmitResult result =
          book.submit({index + 1, Side::Buy, 10'000, 100});
      return result.accepted && result.executed_quantity == 0 &&
             result.resting_quantity == 100;
    });
  }

  finish_throughput_benchmark(state, total, "resting-add", binding);
}

void BM_OrderBook_RandomCancel_Throughput(benchmark::State &state) {
  const auto total = static_cast<std::uint64_t>(state.range(0));
  const CpuBindingResult binding = apply_engine_cpu_binding();

  for (auto _ : state) {
    (void)_;
    OrderBook book(static_cast<std::size_t>(total + 1));
    std::vector<OrderId> cancellation_order(total);
    std::iota(cancellation_order.begin(), cancellation_order.end(), 1);

    bool setup_valid = true;
    for (OrderId order_id : cancellation_order) {
      setup_valid = book.submit({order_id, Side::Buy, 10'000, 100}).accepted &&
                    setup_valid;
    }
    std::mt19937_64 generator(0xC0FFEEULL);
    std::shuffle(cancellation_order.begin(), cancellation_order.end(),
                 generator);

    if (!setup_valid) {
      state.SkipWithError("random-cancel setup failed");
      continue;
    }

    measure_throughput_batches(state, total, [&](std::uint64_t index) {
      return book.cancel(cancellation_order[index]) == CancelResult::Canceled;
    });
  }

  finish_throughput_benchmark(state, total, "random-cancel", binding);
}

void BM_OrderBook_SingleMatch_Throughput(benchmark::State &state) {
  const auto total = static_cast<std::uint64_t>(state.range(0));
  const CpuBindingResult binding = apply_engine_cpu_binding();

  for (auto _ : state) {
    (void)_;
    OrderBook book(static_cast<std::size_t>(total + 1));
    bool setup_valid = true;
    for (OrderId order_id = 1; order_id <= total; ++order_id) {
      setup_valid = book.submit({order_id, Side::Sell, 10'000, 100}).accepted &&
                    setup_valid;
    }
    if (!setup_valid) {
      state.SkipWithError("single-match setup failed");
      continue;
    }

    std::uint64_t trade_count = 0;
    const bool measured =
        measure_throughput_batches(state, total, [&](std::uint64_t index) {
          const SubmitResult result =
              book.submit({total + index + 1, Side::Buy, 10'000, 100},
                          count_trade, &trade_count);
          return result.accepted && result.executed_quantity == 100 &&
                 result.resting_quantity == 0;
        });
    if (measured && trade_count != total) {
      state.SkipWithError("single-match trade-count validation failed");
    }
  }

  finish_throughput_benchmark(state, total, "single-match", binding);
}

void BM_OrderBook_SweepTen_Throughput(benchmark::State &state) {
  constexpr std::uint64_t kMakersPerRequest = 10;
  const auto total = static_cast<std::uint64_t>(state.range(0));
  const std::uint64_t maker_count = total * kMakersPerRequest;
  const CpuBindingResult binding = apply_engine_cpu_binding();

  for (auto _ : state) {
    (void)_;
    OrderBook book(static_cast<std::size_t>(maker_count + 1));
    bool setup_valid = true;
    for (OrderId order_id = 1; order_id <= maker_count; ++order_id) {
      setup_valid = book.submit({order_id, Side::Sell, 10'000, 100}).accepted &&
                    setup_valid;
    }
    if (!setup_valid) {
      state.SkipWithError("sweep-ten setup failed");
      continue;
    }

    std::uint64_t trade_count = 0;
    const bool measured =
        measure_throughput_batches(state, total, [&](std::uint64_t index) {
          const SubmitResult result =
              book.submit({maker_count + index + 1, Side::Buy, 10'000,
                           100 * kMakersPerRequest},
                          count_trade, &trade_count);
          return result.accepted &&
                 result.executed_quantity == 100 * kMakersPerRequest &&
                 result.resting_quantity == 0;
        });
    if (measured && trade_count != maker_count) {
      state.SkipWithError("sweep-ten trade-count validation failed");
    }
  }

  finish_throughput_benchmark(state, total, "sweep-ten", binding);
}

BENCHMARK(BM_OrderBook_RestingAdd_Throughput)
    ->Arg(100'000)
    ->Iterations(1)
    ->UseManualTime();

BENCHMARK(BM_OrderBook_RandomCancel_Throughput)
    ->Arg(100'000)
    ->Iterations(1)
    ->UseManualTime();

BENCHMARK(BM_OrderBook_SingleMatch_Throughput)
    ->Arg(100'000)
    ->Iterations(1)
    ->UseManualTime();

BENCHMARK(BM_OrderBook_SweepTen_Throughput)
    ->Arg(100'000)
    ->Iterations(1)
    ->UseManualTime();

} // namespace
} // namespace orderbook::benchmarking
