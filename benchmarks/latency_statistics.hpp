#pragma once

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace orderbook::benchmarking {

inline std::int64_t percentile(const std::vector<std::int64_t> &sorted,
                               double quantile) {
  const double rank = std::ceil(quantile * static_cast<double>(sorted.size()));
  const std::size_t index =
      rank <= 1.0 ? 0 : static_cast<std::size_t>(rank - 1.0);
  return sorted[std::min(index, sorted.size() - 1)];
}

inline void report_latency(benchmark::State &state,
                           std::vector<std::int64_t> samples) {
  if (samples.empty()) {
    state.SkipWithError("latency sample set is empty");
    return;
  }

  std::sort(samples.begin(), samples.end());
  const long double sum = std::accumulate(samples.begin(), samples.end(),
                                          static_cast<long double>(0));

  state.counters["latency_mean_ns"] = static_cast<double>(sum / samples.size());
  state.counters["latency_p50_ns"] = percentile(samples, 0.50);
  state.counters["latency_p95_ns"] = percentile(samples, 0.95);
  state.counters["latency_p99_ns"] = percentile(samples, 0.99);
  state.counters["latency_p999_ns"] = percentile(samples, 0.999);
  state.counters["latency_max_ns"] = samples.back();
  state.counters["latency_samples"] = static_cast<double>(samples.size());
}

} // namespace orderbook::benchmarking
