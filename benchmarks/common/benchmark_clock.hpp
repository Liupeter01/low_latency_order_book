#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace orderbook::benchmarking {

using BenchmarkClock = std::chrono::steady_clock;

inline std::int64_t benchmark_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             BenchmarkClock::now().time_since_epoch())
      .count();
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__ __volatile__("yield");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Returns the first observed time at or after the deadline.
inline std::int64_t spin_until_ns(std::int64_t deadline_ns) noexcept {
  std::int64_t now_ns = benchmark_now_ns();
  while (now_ns < deadline_ns) {
    cpu_relax();
    now_ns = benchmark_now_ns();
  }
  return now_ns;
}

} // namespace orderbook::benchmarking
