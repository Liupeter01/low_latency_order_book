#include "engine/fixed_rate_arrival_schedule.hpp"

#include <gtest/gtest.h>

namespace orderbook::benchmarking {
namespace {

TEST(FixedRateArrivalSchedule, ComputesScheduledTimeFromStartAndRate) {
  const FixedRateArrivalSchedule schedule(100, 4);

  EXPECT_EQ(schedule.scheduled_ns(0), 100);
  EXPECT_EQ(schedule.scheduled_ns(1), 250'000'100);
  EXPECT_EQ(schedule.scheduled_ns(4), 1'000'000'100);
  EXPECT_EQ(schedule.scheduled_ns(5), 1'250'000'100);
  EXPECT_EQ(schedule.interval_ns(), 250'000'000);
}

TEST(FixedRateArrivalSchedule, FractionalIntervalsDoNotAccumulateDrift) {
  const FixedRateArrivalSchedule thirds(100, 3);
  EXPECT_EQ(thirds.scheduled_ns(0), 100);
  EXPECT_EQ(thirds.scheduled_ns(1), 333'333'433);
  EXPECT_EQ(thirds.scheduled_ns(2), 666'666'766);
  EXPECT_EQ(thirds.scheduled_ns(3), 1'000'000'100);
  EXPECT_EQ(thirds.scheduled_ns(3'000), 1'000'000'000'100);
  EXPECT_EQ(thirds.interval_ns(), 333'333'334);

  const FixedRateArrivalSchedule fast(0, 72'000'000);
  EXPECT_EQ(fast.scheduled_ns(1), 13);
  EXPECT_EQ(fast.scheduled_ns(72), 1'000);
  EXPECT_EQ(fast.scheduled_ns(72'000'000), 1'000'000'000);
  EXPECT_EQ(fast.interval_ns(), 14);
}

TEST(FixedRateArrivalSchedule, DetectsOneIntervalOfProducerLateness) {
  const FixedRateArrivalSchedule schedule(1'000, 1'000'000);
  const std::int64_t scheduled_ns = schedule.scheduled_ns(7);
  EXPECT_LT(scheduled_ns + 999 - scheduled_ns, schedule.interval_ns());
  EXPECT_GE(scheduled_ns + 1'000 - scheduled_ns, schedule.interval_ns());
}

TEST(FixedRateArrivalSchedule, RejectsZeroAndSubNanosecondRates) {
  EXPECT_THROW(FixedRateArrivalSchedule(0, 0), std::invalid_argument);
  EXPECT_THROW(FixedRateArrivalSchedule(0, 1'000'000'001),
               std::invalid_argument);
}

} // namespace
} // namespace orderbook::benchmarking
