#pragma once

#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#endif

namespace orderbook::spinbackoff {
struct SpinBackoff {
  SpinBackoff() noexcept = default;
  void reset() noexcept { attempt_ = 0; }

  void wait() noexcept {
    const std::uint32_t shift =
        attempt_ < kMaximumPauseShift ? attempt_ : kMaximumPauseShift;
    const std::uint32_t count = std::uint32_t{1} << shift;

    for (std::uint32_t i = 0; i < count; ++i) {
      cpu_relax();
    }

    if (attempt_ < kMaximumPauseShift) {
      ++attempt_;
    }
  }

  // Precise time-based waits should use one processor hint rather than the
  // adaptive wait(), whose eventual OS yield can overshoot a scheduled time.
  static void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||           \
    defined(_M_IX86)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
  }

private:
  static constexpr std::uint32_t kMaximumPauseShift = 3;
  std::uint32_t attempt_{};
};
} // namespace orderbook::spinbackoff
