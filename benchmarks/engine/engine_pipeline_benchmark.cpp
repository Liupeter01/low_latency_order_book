#include "common/benchmark_clock.hpp"
#include "common/cpu_affinity.hpp"
#include "common/latency_statistics.hpp"
#include "engine/fixed_rate_arrival_schedule.hpp"
#include "engine/small_book_workload.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace orderbook::benchmarking {
namespace {

constexpr std::size_t kChannelCapacity = 4096;
constexpr std::uint64_t kWarmupRequests = 20'000;
// Coprime to the four-request cycle: every operation type is sampled.
constexpr std::uint64_t kSampleStride = 127;

struct alignas(64) CompletionCount {
  std::atomic<std::uint64_t> value{0};
};

struct Sample {
  std::int64_t scheduled_ns{};
  std::int64_t submit_start_ns{};
  std::int64_t enqueue_attempt_ns{};
  std::int64_t completed_ns{};
};

struct RunResult {
  bool valid{true};
  std::int64_t start_ns{};
  std::int64_t last_enqueue_ns{};
  std::int64_t completed_ns{};
  std::int64_t window_end_ns{};
  std::int64_t window_observation_lag_ns{};
  std::uint64_t pending_at_window_observation{};
  std::uint64_t unsent_at_window_observation{};
  std::uint64_t pending_after_last_enqueue{};
  std::uint64_t request_queue_full_retries{};
  std::uint64_t late_by_interval{};
  std::uint64_t measured_events{};
  std::uint64_t completed_requests{};
  std::uint64_t in_flight_sampled_max{};
  long double in_flight_sampled_sum{};
  std::uint64_t event_depth_sampled_max{};
  long double event_depth_sampled_sum{};
  std::vector<Sample> samples;
  std::string affinity;

  double seconds() const noexcept {
    return static_cast<double>(std::max(completed_ns, window_end_ns) -
                               start_ns) /
           1'000'000'000.0;
  }
  double requests_per_second() const noexcept {
    return static_cast<double>(completed_requests) / seconds();
  }
};

std::uint64_t read_setting(const char *name, std::uint64_t fallback,
                           std::uint64_t maximum) {
  const char *text = std::getenv(name);
  if (text == nullptr) {
    return fallback;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (*text < '0' || *text > '9' || end == text || *end != '\0' || errno ||
      value == 0 || value > maximum) {
    throw std::invalid_argument(std::string("invalid ") + name);
  }
  return static_cast<std::uint64_t>(value);
}

std::uint64_t measured_request_count() {
  const auto count =
      read_setting("ORDERBOOK_BENCHMARK_REQUESTS", 200'000, 10'000'000);
  if (count < SmallBookWorkload::kRequestsPerCycle ||
      count % SmallBookWorkload::kRequestsPerCycle != 0) {
    throw std::invalid_argument(
        "request count must be a positive multiple of 4");
  }
  return count;
}

RunResult run_pipeline(std::uint64_t measured, std::uint64_t target_rate = 0) {
  static_assert(std::chrono::steady_clock::is_steady,
                "pipeline timing requires a monotonic clock");
  const bool open_loop = target_rate != 0;
  const std::uint64_t total = kWarmupRequests + measured;
  const SmallBookWorkload workload;
  std::vector<OrderRequest> flow;
  flow.reserve(total);
  for (std::uint64_t index = 0; index < total; ++index) {
    flow.push_back(workload.request(index));
  }

  // Allocate and touch metadata and transport storage before timing.
  RunResult result;
  if (open_loop) {
    result.samples.resize((measured + kSampleStride - 1) / kSampleStride);
  }
  auto requests = std::make_unique<RequestChannel<kChannelCapacity>>();
  auto events = std::make_unique<EventChannel<kChannelCapacity>>();
  MatchingEngine<kChannelCapacity, kChannelCapacity> engine(
      requests->make_reader(), events->make_writer(), 1024);
  auto writer = requests->make_writer();
  OrderEventReader<kChannelCapacity> reader(events->make_reader());
  CompletionCount completed;
  alignas(64) std::atomic<unsigned> ready{0};
  alignas(64) std::atomic<bool> start{false};
  CpuBindingResult producer_binding, engine_binding, consumer_binding;

  const auto await_start = [&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      cpu_relax();
    }
  };

  std::thread engine_thread([&] {
    engine_binding =
        bind_current_thread_from_environment("ORDERBOOK_BENCHMARK_ENGINE_CPU");
    await_start();
    engine.run();
  });

  std::thread consumer([&, reader = std::move(reader)]() mutable {
    consumer_binding = bind_current_thread_from_environment(
        "ORDERBOOK_BENCHMARK_CONSUMER_CPU");
    await_start();
    std::uint64_t index = 0;
    std::uint64_t trades = 0;
    for (;;) {
      OrderEvent event;
      if (!reader.try_read(event)) {
        cpu_relax();
        continue;
      }
      const bool measured_request = index >= kWarmupRequests && index < total;
      const bool sampled = open_loop && measured_request &&
                           (index - kWarmupRequests) % kSampleStride == 0;
      const bool terminal = event.type != OrderEventType::Trade &&
                            event.type != OrderEventType::EngineStopped;
      // Capture before validation and histogram work. Trade events are not
      // completion: the final acknowledgment follows all fills for the order.
      const auto received_ns =
          terminal && (sampled || index + 1 == total) ? benchmark_now_ns() : 0;
      if (event.type == OrderEventType::EngineStopped) {
        result.valid = result.valid && event.sequence == total + 1 &&
                       index == total && trades == 0;
        break;
      }
      if (index >= total) {
        result.valid = false;
        continue;
      }
      if (measured_request) {
        ++result.measured_events;
      }
      if (event.type == OrderEventType::Trade) {
        result.valid = workload.validate_trade(event, index) && result.valid;
        ++trades;
        continue;
      }
      if (sampled) {
        result.samples[(index - kWarmupRequests) / kSampleStride].completed_ns =
            received_ns;
        const auto depth = reader.approximate_size();
        result.event_depth_sampled_max = std::max(
            result.event_depth_sampled_max, static_cast<std::uint64_t>(depth));
        result.event_depth_sampled_sum += depth;
      }
      result.valid =
          workload.validate_completion(event, index, trades) && result.valid;
      trades = 0;
      ++index;
      if (index == total) {
        result.completed_ns = received_ns;
      }
      completed.value.store(index, std::memory_order_release);
    }
    result.completed_requests =
        index >= kWarmupRequests ? index - kWarmupRequests : 0;
  });

  std::thread producer([&, writer = std::move(writer)]() mutable {
    producer_binding = bind_current_thread_from_environment(
        "ORDERBOOK_BENCHMARK_PRODUCER_CPU");
    await_start();
    for (std::uint64_t index = 0; index < kWarmupRequests; ++index) {
      while (!writer.try_write(flow[index])) {
        cpu_relax();
      }
    }
    while (completed.value.load(std::memory_order_acquire) < kWarmupRequests) {
      cpu_relax();
    }

    // Warmup finishes on a cycle boundary with an empty book and drained
    // output.
    result.start_ns = benchmark_now_ns();
    const FixedRateArrivalSchedule schedule(result.start_ns,
                                            open_loop ? target_rate : 1);
    // measured is the time slot immediately after the final zero-based index.
    result.window_end_ns =
        open_loop ? schedule.scheduled_ns(measured) : result.start_ns;
    std::uint64_t sent = 0;
    bool window_observed = false;
    const auto completed_measured = [&] {
      return std::min(measured,
                      completed.value.load(std::memory_order_acquire) -
                          kWarmupRequests);
    };
    const auto observe_window = [&](std::int64_t now) {
      if (!window_observed && now >= result.window_end_ns) {
        result.pending_at_window_observation = measured - completed_measured();
        result.unsent_at_window_observation = measured - sent;
        result.window_observation_lag_ns = now - result.window_end_ns;
        window_observed = true;
      }
    };

    for (std::uint64_t index = 0; index < measured; ++index) {
      const auto &request = flow[kWarmupRequests + index];
      if (!open_loop) {
        while (!writer.try_write(request)) {
          ++result.request_queue_full_retries;
          cpu_relax();
        }
        ++sent;
        continue;
      }

      const auto scheduled_ns = schedule.scheduled_ns(index);
      const auto now = spin_until_ns(scheduled_ns);
      observe_window(now);
      if (now - scheduled_ns >= schedule.interval_ns()) {
        ++result.late_by_interval;
      }
      Sample *sample = index % kSampleStride == 0
                           ? &result.samples[index / kSampleStride]
                           : nullptr;
      if (sample != nullptr) {
        sample->scheduled_ns = scheduled_ns;
        sample->submit_start_ns = now;
        const auto in_flight = sent - completed_measured();
        result.in_flight_sampled_max =
            std::max(result.in_flight_sampled_max, in_flight);
        result.in_flight_sampled_sum += in_flight;
      }
      for (;;) {
        if (sample != nullptr) {
          sample->enqueue_attempt_ns = benchmark_now_ns();
        }
        if (writer.try_write(request)) {
          break;
        }
        ++result.request_queue_full_retries;
        observe_window(benchmark_now_ns());
        cpu_relax();
      }
      // Producer metadata is immutable after publication. The two SPSC
      // release/acquire handoffs make it visible to the event consumer.
      ++sent;
    }
    result.last_enqueue_ns = benchmark_now_ns();
    result.pending_after_last_enqueue = measured - completed_measured();
    if (open_loop) {
      auto now = result.last_enqueue_ns;
      while (!window_observed) {
        observe_window(now);
        if (!window_observed) {
          cpu_relax();
          now = benchmark_now_ns();
        }
      }
    }
    const auto stop = OrderRequest::shutdown(total + 1);
    while (!writer.try_write(stop)) {
      cpu_relax();
    }
  });

  while (ready.load(std::memory_order_acquire) != 3) {
    cpu_relax();
  }
  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  engine_thread.join();
  result.affinity = binding_description("producer", producer_binding) + ";" +
                    binding_description("engine", engine_binding) + ";" +
                    binding_description("consumer", consumer_binding);
  result.valid =
      result.valid && result.completed_requests == measured &&
      result.measured_events == measured * SmallBookWorkload::kEventsPerCycle /
                                    SmallBookWorkload::kRequestsPerCycle &&
      result.completed_ns > result.start_ns;
  return result;
}

void report_samples(benchmark::State &state, const RunResult &result) {
  std::vector<std::int64_t> scheduled, admitted, submit_lag, admission_wait;
  for (auto *values : {&scheduled, &admitted, &submit_lag, &admission_wait}) {
    values->reserve(result.samples.size());
  }
  for (const auto &sample : result.samples) {
    if (sample.submit_start_ns < sample.scheduled_ns ||
        sample.enqueue_attempt_ns < sample.submit_start_ns ||
        sample.completed_ns < sample.enqueue_attempt_ns) {
      state.SkipWithError("invalid pipeline timestamp ordering");
      return;
    }
    scheduled.push_back(sample.completed_ns - sample.scheduled_ns);
    admitted.push_back(sample.completed_ns - sample.enqueue_attempt_ns);
    submit_lag.push_back(sample.submit_start_ns - sample.scheduled_ns);
    admission_wait.push_back(sample.enqueue_attempt_ns -
                             sample.submit_start_ns);
  }
  report_nanosecond_distribution(state, std::move(scheduled),
                                 "scheduled_latency",
                                 "no pipeline latency samples");
  report_nanosecond_distribution(state, std::move(admitted), "admitted_latency",
                                 "no pipeline latency samples");
  report_nanosecond_distribution(state, std::move(submit_lag), "submit_lag",
                                 "no pipeline latency samples");
  report_nanosecond_distribution(state, std::move(admission_wait),
                                 "admission_wait",
                                 "no pipeline latency samples");
}

void report_run(benchmark::State &state, const RunResult &result,
                std::uint64_t measured, std::uint64_t target_rate) {
  state.SetIterationTime(result.seconds());
  if (!result.valid) {
    state.SkipWithError("engine pipeline event/count validation failed");
    return;
  }
  state.counters["completed_requests_per_second"] =
      result.requests_per_second();
  state.counters["events_per_second"] =
      result.measured_events / result.seconds();
  state.counters["request_queue_full_retries"] =
      result.request_queue_full_retries;
  state.counters["pending_after_last_enqueue"] =
      result.pending_after_last_enqueue;
  state.counters["remaining_after_drain"] =
      measured - result.completed_requests;
  state.counters["request_bytes"] = sizeof(OrderRequest);
  state.counters["event_bytes"] = sizeof(OrderEvent);
  state.counters["channel_capacity"] = kChannelCapacity;
  if (target_rate != 0) {
    report_samples(state, result);
    state.counters["target_requests_per_second"] = target_rate;
    state.counters["achieved_target_percent"] =
        100.0 * result.requests_per_second() / target_rate;
    state.counters["producer_late_by_interval_percent"] =
        100.0 * result.late_by_interval / measured;
    state.counters["pending_at_window_observation"] =
        result.pending_at_window_observation;
    state.counters["unsent_at_window_observation"] =
        result.unsent_at_window_observation;
    state.counters["window_observation_lag_ns"] =
        result.window_observation_lag_ns;
    state.counters["drain_after_window_ns"] =
        std::max<std::int64_t>(0, result.completed_ns - result.window_end_ns);
    state.counters["in_flight_sampled_max"] = result.in_flight_sampled_max;
    state.counters["in_flight_sampled_mean"] = static_cast<double>(
        result.in_flight_sampled_sum / result.samples.size());
    state.counters["event_depth_sampled_max"] = result.event_depth_sampled_max;
    state.counters["event_depth_sampled_mean"] = static_cast<double>(
        result.event_depth_sampled_sum / result.samples.size());
  }
  state.SetLabel(
      "mixed 4-request cycle: resting sell, lower resting buy, matching buy, "
      "cancel; 5 events/cycle; warmup=" +
      std::to_string(kWarmupRequests) +
      (target_rate ? "; fixed arrivals; sample stride=127; "
                   : "; unpaced pipeline; ") +
      result.affinity);
}

void BM_Engine_Mixed_Throughput(benchmark::State &state) {
  try {
    const auto measured = measured_request_count();
    for (auto _ : state) {
      (void)_;
      const auto result = run_pipeline(measured);
      report_run(state, result, measured, 0);
    }
    state.SetItemsProcessed(
        static_cast<std::int64_t>(state.iterations() * measured));
  } catch (const std::exception &error) {
    state.SkipWithError(error.what());
  }
}

void BM_Engine_Mixed_OpenLoop(benchmark::State &state) {
  try {
    const auto measured = measured_request_count();
    const auto explicit_rate = std::getenv("ORDERBOOK_BENCHMARK_TARGET_RPS");
    std::uint64_t target = 0;
    double reference = 0;
    double reference_cv = 0;
    if (explicit_rate != nullptr) {
      target = read_setting("ORDERBOOK_BENCHMARK_TARGET_RPS", 1, 1'000'000'000);
    } else {
      std::vector<double> rates;
      for (unsigned trial = 0; trial < 3; ++trial) {
        const auto calibration = run_pipeline(measured);
        if (!calibration.valid) {
          state.SkipWithError("throughput calibration failed");
          return;
        }
        rates.push_back(calibration.requests_per_second());
      }
      std::sort(rates.begin(), rates.end());
      reference = rates[1];
      const auto mean = (rates[0] + rates[1] + rates[2]) / 3;
      double variance = 0;
      for (double rate : rates) {
        variance += (rate - mean) * (rate - mean);
      }
      reference_cv = 100.0 * std::sqrt(variance / 3) / mean;
      target = static_cast<std::uint64_t>(std::max(
          1.0, std::min(1'000'000'000.0, reference * state.range(0) / 100.0)));
    }
    for (auto _ : state) {
      (void)_;
      const auto result = run_pipeline(measured, target);
      report_run(state, result, measured, target);
    }
    state.counters["explicit_target"] = explicit_rate != nullptr ? 1 : 0;
    if (explicit_rate == nullptr) {
      state.counters["reference_requests_per_second"] = reference;
      state.counters["reference_cv_percent"] = reference_cv;
      state.counters["offered_reference_percent"] = state.range(0);
    }
    state.SetItemsProcessed(
        static_cast<std::int64_t>(state.iterations() * measured));
  } catch (const std::exception &error) {
    state.SkipWithError(error.what());
  }
}

BENCHMARK(BM_Engine_Mixed_Throughput)
    ->Iterations(1)
    ->Repetitions(5)
    ->ReportAggregatesOnly(true)
    ->UseManualTime();

BENCHMARK(BM_Engine_Mixed_OpenLoop)
    ->Arg(50)
    ->Arg(70)
    ->Arg(85)
    ->Arg(95)
    ->Iterations(1)
    ->UseManualTime();

} // namespace
} // namespace orderbook::benchmarking
