#include "orderbook/spsc_channel.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

namespace orderbook::test {
namespace {

struct TestMessage {
  std::uint64_t sequence{};
  std::uint64_t value{};
};

TEST(SpscChannel, PreservesFifoOrderAndReportsFull) {
  SpscChannel<TestMessage, 4> channel;
  auto writer = channel.make_writer();
  auto reader = channel.make_reader();

  EXPECT_TRUE(writer.try_write({1, 11}));
  EXPECT_TRUE(writer.try_write({2, 22}));
  EXPECT_TRUE(writer.try_write({3, 33}));
  EXPECT_TRUE(writer.try_write({4, 44}));
  EXPECT_FALSE(writer.try_write({5, 55}));

  for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
    TestMessage message;
    ASSERT_TRUE(reader.try_read(message));
    EXPECT_EQ(message.sequence, sequence);
    EXPECT_EQ(message.value, sequence * 11);
  }

  TestMessage message;
  EXPECT_FALSE(reader.try_read(message));
}

TEST(SpscChannel, TransfersMessagesBetweenTwoThreads) {
  constexpr std::uint64_t kMessageCount = 100'000;
  SpscChannel<TestMessage, 1024> channel;
  auto writer = channel.make_writer();
  auto reader = channel.make_reader();

  std::thread producer([writer = std::move(writer)]() mutable {
    for (std::uint64_t sequence = 1; sequence <= kMessageCount; ++sequence) {
      const TestMessage message{sequence, sequence};
      while (!writer.try_write(message)) {
        std::this_thread::yield();
      }
    }
  });

  for (std::uint64_t sequence = 1; sequence <= kMessageCount; ++sequence) {
    TestMessage message;
    while (!reader.try_read(message)) {
      std::this_thread::yield();
    }
    ASSERT_EQ(message.sequence, sequence);
    ASSERT_EQ(message.value, sequence);
  }

  producer.join();
}

} // namespace
} // namespace orderbook::test
