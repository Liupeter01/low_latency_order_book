#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace orderbook {

// The channel exposes two different endpoint types. A writer cannot read and a
// reader cannot write, so transport direction is explicit at the type level.
template <typename T, std::size_t Capacity> class SpscChannel {
  static_assert(Capacity >= 2, "SPSC capacity must be at least two");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "SPSC capacity must be a power of two");
  static_assert(std::is_trivially_copyable<T>::value,
                "This channel intentionally transports flat messages");

public:
  class Writer {
  public:
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    Writer(Writer &&) noexcept = default;
    Writer &operator=(Writer &&) noexcept = default;

    bool try_write(const T &value) noexcept {
      return channel_->try_write(value);
    }

  private:
    friend class SpscChannel;
    explicit Writer(SpscChannel &channel) noexcept : channel_(&channel) {}
    SpscChannel *channel_;
  };

  class Reader {
  public:
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    Reader(Reader &&) noexcept = default;
    Reader &operator=(Reader &&) noexcept = default;

    bool try_read(T &value) noexcept { return channel_->try_read(value); }

    std::size_t approximate_size() const noexcept {
      return channel_->approximate_size();
    }

  private:
    friend class SpscChannel;
    explicit Reader(SpscChannel &channel) noexcept : channel_(&channel) {}
    SpscChannel *channel_;
  };

  Writer make_writer() noexcept { return Writer(*this); }
  Reader make_reader() noexcept { return Reader(*this); }

private:
  bool try_write(const T &value) noexcept {
    const std::uint64_t write = write_position_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_position_.load(std::memory_order_acquire);
    if (write - read == Capacity) {
      return false;
    }

    slots_[write & kMask] = value;
    write_position_.store(write + 1, std::memory_order_release);
    return true;
  }

  bool try_read(T &value) noexcept {
    const std::uint64_t read = read_position_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_position_.load(std::memory_order_acquire);
    if (read == write) {
      return false;
    }

    value = slots_[read & kMask];
    read_position_.store(read + 1, std::memory_order_release);
    return true;
  }

  std::size_t approximate_size() const noexcept {
    const std::uint64_t write = write_position_.load(std::memory_order_acquire);
    const std::uint64_t read = read_position_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
  }

  static constexpr std::uint64_t kMask = Capacity - 1;

  alignas(64) std::array<T, Capacity> slots_{};
  alignas(64) std::atomic<std::uint64_t> write_position_{0};
  alignas(64) std::atomic<std::uint64_t> read_position_{0};
};

} // namespace orderbook
