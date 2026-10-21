#include "latency_statistics.hpp"
#include "cpu_affinity.hpp"

#include "orderbook/spsc_channel.hpp"
#include "orderbook/types.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace orderbook::benchmarking {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kWarmupItems = 100'000;
constexpr std::uint64_t kSampleStride = 100;
constexpr std::size_t kChannelCapacity = 1024;

struct CompactHandoffValue {
    std::uint64_t sequence{};
    std::int64_t published_ns{};
};

static_assert(sizeof(OrderRequest) >= sizeof(CompactHandoffValue),
              "OrderRequest must fit sequence and timestamp fields");

struct alignas(alignof(OrderRequest)) OrderRequestSizedHandoffValue {
    std::uint64_t sequence{};
    std::int64_t published_ns{};
    std::array<std::byte,
               sizeof(OrderRequest) - sizeof(CompactHandoffValue)> payload{};
};

static_assert(std::is_trivially_copyable<CompactHandoffValue>::value,
              "compact handoff payload must remain trivially copyable");
static_assert(std::is_trivially_copyable<OrderRequestSizedHandoffValue>::value,
              "request-sized handoff payload must remain trivially copyable");
static_assert(sizeof(OrderRequestSizedHandoffValue) == sizeof(OrderRequest),
              "benchmark payload must match OrderRequest size");
static_assert(alignof(OrderRequestSizedHandoffValue) == alignof(OrderRequest),
              "benchmark payload must match OrderRequest alignment");

struct alignas(64) PaddedSequence {
    std::atomic<std::uint64_t> value{0};
};

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ __volatile__("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

bool should_sample(std::uint64_t sequence) noexcept {
    return sequence >= kWarmupItems &&
           ((sequence - kWarmupItems) % kSampleStride == 0);
}

std::size_t expected_sample_count(std::uint64_t total_items) noexcept {
    if (total_items <= kWarmupItems) {
        return 0;
    }
    return static_cast<std::size_t>(
        (total_items - kWarmupItems + kSampleStride - 1) / kSampleStride);
}

template <typename HandoffValue>
void run_one_in_flight_handoff(benchmark::State& state) {
    static_assert(Clock::is_steady,
                  "handoff latency requires a monotonic steady clock");

    const auto total_items = static_cast<std::uint64_t>(state.range(0));
    const std::size_t expected_samples = expected_sample_count(total_items);
    std::string affinity_label;

    for (auto _ : state) {
        (void)_;
        SpscChannel<HandoffValue, kChannelCapacity> channel;
        auto writer = channel.make_writer();
        auto reader = channel.make_reader();

        alignas(64) std::atomic<std::size_t> ready{0};
        alignas(64) std::atomic<bool> start{false};
        PaddedSequence consumed_sequence;
        bool valid = true;
        CpuBindingResult producer_binding;
        CpuBindingResult consumer_binding;

        std::vector<std::int64_t> samples;
        samples.reserve(expected_samples);

        std::thread producer(
            [&, writer = std::move(writer)]() mutable {
                producer_binding = bind_current_thread_from_environment(
                    "ORDERBOOK_BENCHMARK_PRODUCER_CPU");
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    cpu_relax();
                }

                for (std::uint64_t sequence = 0;
                     sequence < total_items; ++sequence) {
                    while (consumed_sequence.value.load(
                               std::memory_order_acquire) != sequence) {
                        cpu_relax();
                    }

                    HandoffValue value{};
                    value.sequence = sequence;
                    value.published_ns =
                        should_sample(sequence) ? now_ns() : 0;
                    while (!writer.try_write(value)) {
                        cpu_relax();
                    }
                }
            });

        std::thread consumer(
            [&, reader = std::move(reader)]() mutable {
                consumer_binding = bind_current_thread_from_environment(
                    "ORDERBOOK_BENCHMARK_CONSUMER_CPU");
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    cpu_relax();
                }

                for (std::uint64_t expected = 0;
                     expected < total_items; ++expected) {
                    HandoffValue value;
                    while (!reader.try_read(value)) {
                        cpu_relax();
                    }

                    if (value.sequence != expected) {
                        valid = false;
                    }

                    if (value.published_ns != 0) {
                        const std::int64_t received_ns = now_ns();
                        if (received_ns < value.published_ns) {
                            valid = false;
                        } else {
                            samples.push_back(received_ns - value.published_ns);
                        }
                    }

                    consumed_sequence.value.store(expected + 1,
                                                  std::memory_order_release);
                }
            });

        while (ready.load(std::memory_order_acquire) != 2) {
            cpu_relax();
        }

        const auto begin = Clock::now();
        start.store(true, std::memory_order_release);

        consumer.join();
        const auto end = Clock::now();
        producer.join();

        affinity_label = binding_description("producer", producer_binding) +
                         ";" +
                         binding_description("consumer", consumer_binding);

        if (binding_failed(producer_binding) ||
            binding_failed(consumer_binding)) {
            state.SkipWithError("requested SPSC CPU binding failed");
            continue;
        }

        state.SetIterationTime(
            std::chrono::duration<double>(end - begin).count());

        if (!valid || samples.size() != expected_samples) {
            state.SkipWithError("SPSC handoff validation failed");
            continue;
        }

        report_latency(state, std::move(samples));
    }

    state.SetItemsProcessed(
        static_cast<std::int64_t>(state.iterations() * total_items));
    state.counters["payload_bytes"] = sizeof(HandoffValue);
    state.SetLabel(std::to_string(total_items) +
                   " handoffs; one item in flight; 1/100 sampled after " +
                   std::to_string(kWarmupItems) + " warmup; payload=" +
                   std::to_string(sizeof(HandoffValue)) + "B; " +
                   affinity_label);
}

void BM_SpscChannel_CompactHandoff(benchmark::State& state) {
    run_one_in_flight_handoff<CompactHandoffValue>(state);
}

void BM_SpscChannel_OrderRequestSizedHandoff(benchmark::State& state) {
    run_one_in_flight_handoff<OrderRequestSizedHandoffValue>(state);
}

BENCHMARK(BM_SpscChannel_CompactHandoff)
    ->Arg(1'000'000)
    ->Iterations(1)
    ->UseManualTime();

BENCHMARK(BM_SpscChannel_OrderRequestSizedHandoff)
    ->Arg(1'000'000)
    ->Iterations(1)
    ->UseManualTime();

}  // namespace
}  // namespace orderbook::benchmarking
