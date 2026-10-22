#pragma once

#include <cstdint>
#include <stdexcept>

namespace orderbook::benchmarking {

// Produces absolute arrival times for a zero-based request sequence at a fixed
// requests-per-second rate.
class FixedRateArrivalSchedule {
public:
  static constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

  FixedRateArrivalSchedule(std::int64_t start_ns,
                           std::uint64_t target_requests_per_second)
      : start_ns_(start_ns),
        target_requests_per_second_(target_requests_per_second) {
    if (target_requests_per_second_ == 0 ||
        target_requests_per_second_ > kNanosecondsPerSecond) {
      throw std::invalid_argument("target rate must be in [1, 1e9] requests/s");
    }
  }

  // Absolute scheduled time for request_index. The benchmark limits the index
  // to 10M, so the multiplication remains inside uint64_t. Computing from the
  // original start avoids accumulating rounded integer intervals.
  std::int64_t scheduled_ns(std::uint64_t request_index) const noexcept {
    return start_ns_ + static_cast<std::int64_t>(
                           request_index * kNanosecondsPerSecond /
                           target_requests_per_second_);
  }

  // One request interval rounded upward, used only as the lateness threshold.
  std::int64_t interval_ns() const noexcept {
    return static_cast<std::int64_t>(
        (kNanosecondsPerSecond + target_requests_per_second_ - 1) /
        target_requests_per_second_);
  }

private:
  std::int64_t start_ns_;
  std::uint64_t target_requests_per_second_;
};

} // namespace orderbook::benchmarking
