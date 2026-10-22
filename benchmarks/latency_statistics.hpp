#pragma once

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace orderbook::benchmarking {

inline std::int64_t percentile(const std::vector<std::int64_t> &sorted,
                               double quantile) {
  const double rank = std::ceil(quantile * static_cast<double>(sorted.size()));
  const std::size_t index =
      rank <= 1.0 ? 0 : static_cast<std::size_t>(rank - 1.0);
  return sorted[std::min(index, sorted.size() - 1)];
}

inline void report_nanosecond_distribution(benchmark::State &state,
                                           std::vector<std::int64_t> samples,
                                           const char *counter_prefix,
                                           const char *empty_sample_error) {
  if (samples.empty()) {
    state.SkipWithError(empty_sample_error);
    return;
  }

  std::sort(samples.begin(), samples.end());
  const long double sum = std::accumulate(samples.begin(), samples.end(),
                                          static_cast<long double>(0));
  const long double mean = sum / samples.size();
  long double squared_deviation_sum = 0;
  for (const std::int64_t sample : samples) {
    const long double deviation = static_cast<long double>(sample) - mean;
    squared_deviation_sum += deviation * deviation;
  }
  const long double standard_deviation =
      std::sqrt(squared_deviation_sum / samples.size());
  const long double coefficient_of_variation =
      mean == 0 ? 0 : standard_deviation / mean;
  const std::string prefix(counter_prefix);

  state.counters[prefix + "_mean_ns"] = static_cast<double>(mean);
  state.counters[prefix + "_stddev_ns"] =
      static_cast<double>(standard_deviation);
  state.counters[prefix + "_cv_percent"] =
      static_cast<double>(coefficient_of_variation * 100);
  state.counters[prefix + "_p50_ns"] = percentile(samples, 0.50);
  state.counters[prefix + "_p95_ns"] = percentile(samples, 0.95);
  state.counters[prefix + "_p99_ns"] = percentile(samples, 0.99);
  state.counters[prefix + "_p999_ns"] = percentile(samples, 0.999);
  state.counters[prefix + "_max_ns"] = samples.back();
  state.counters[prefix + "_samples"] = static_cast<double>(samples.size());
}

inline void report_latency(benchmark::State &state,
                           std::vector<std::int64_t> samples) {
  report_nanosecond_distribution(state, std::move(samples), "latency",
                                 "latency sample set is empty");
}

inline void report_batch_averages(benchmark::State &state,
                                  std::vector<std::int64_t> samples) {
  report_nanosecond_distribution(state, std::move(samples), "batch_avg",
                                 "batch-average sample set is empty");
}

} // namespace orderbook::benchmarking
