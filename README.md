# Low-Latency-Order-Book

## 0x00 Description

**Low-Latency-Order-Book** is a C++17 limit-order-book and matching-engine prototype focused on deterministic ownership, cache-conscious price lookup, bounded message passing, and measurable latency under controlled load.

The project separates the matching core from its threaded transport:

- `OrderBook` implements price-time-priority matching and cancellation.
- `MatchingEngine` is the sole writer to the book.
- Two bounded SPSC channels carry flat request and event messages.
- GoogleTest validates matching rules and transport behavior.
- Google Benchmark measures the queue, the book, and the complete pipeline independently.

This is a systems-performance project and benchmark harness, not a production-ready exchange or trading platform.

### Features

- **Price-Time Priority**: Best price first, followed by FIFO arrival order at the same price.
- **Integer Representation**: Prices and quantities use signed 64-bit integers; matching never compares floating-point values.
- **64-Level Price Blocks**: Consecutive prices share one block and a 64-bit occupancy bitmap.
- **Fast Best-Price Selection**: Bit scans select the highest bid or lowest ask inside the best active block.
- **Intrusive Price-Level FIFO**: Orders at one price are linked by indices stored in preallocated order nodes.
- **Fixed-Capacity Order Pool**: Filled and canceled order slots are recycled.
- **Indexed Cancellation**: An order-ID index locates active orders before unlinking them from their price level.
- **Single-Writer Engine**: Mutable order-book state needs no internal locks or atomics.
- **Directional Interfaces**: Request writers cannot read events, and event readers cannot submit requests.
- **Open-Loop Benchmarking**: Fixed-rate arrivals expose producer lateness, queue pressure, backlog, drain time, and latency percentiles.

### Current Matching Operations

- Add a limit order.
- Match across one or more resting orders.
- Preserve FIFO priority within a price level.
- Rest the unfilled quantity at the incoming order's original limit price.
- Cancel an active order by ID.
- Reject invalid, duplicate, or over-capacity orders.
- Emit trade and terminal order-state events.
- Stop the threaded engine through an ordered shutdown request.

---

## 0x01 Architecture and Data Layout

### Threaded Pipeline

```text
Order Producer
      |
      | OrderRequest (40 bytes)
      v
Bounded SPSC Request Channel
      |
      v
MatchingEngine Thread ---- owns ----> OrderBook
      |
      | OrderEvent (64 bytes)
      v
Bounded SPSC Event Channel
      |
      v
Event Consumer
```

The engine thread is the only thread allowed to mutate the book. The transport types make this direction explicit:

- `OrderRequestWriter` submits, cancels, and requests shutdown.
- `MatchingEngine` consumes requests and produces events.
- `OrderEventReader` consumes trades and acknowledgments.

The current design is SPSC because the measured pipeline has one producer. A multi-producer deployment would require one sequenced SPSC stream per producer or a bounded MPSC ingress queue.

### Order-Book Structure

```text
buy side / sell side
    |
    +-- std::map<BlockNumber, PriceBlock>
            |
            +-- 64-bit occupied-level bitmap
            +-- 64 directly indexed PriceLevel objects
                    |
                    +-- intrusive FIFO of OrderNode indices

order ID -- average O(1) index lookup --> OrderNode
```

Only non-empty price blocks remain in the ordered tree. Locating or creating a block is O(log B), where B is the number of active blocks. Selecting a price inside the chosen block is a constant-size bitmap operation. The node pool reuses order storage, although the separate `std::unordered_map` order index may still allocate internally.

### Matching Rules

1. A buy crosses while its limit is greater than or equal to the best ask.
2. A sell crosses while its limit is less than or equal to the best bid.
3. Better prices execute before worse prices.
4. Orders at one price execute in FIFO order.
5. A partially filled incoming order rests at its original limit.

`PriceTick` and `Quantity` are signed 64-bit integers. With cent ticks, `$99.95` is represented as `9995`. If quantities use four decimal places, `10.25` shares is represented as `102500`.

---

## 0x02 Developer Quick Start

### Platform Support

The core is portable C++17 and builds on Linux, macOS, and Windows with a conforming compiler. Exact benchmark CPU-ID binding is implemented for Linux. macOS runs are labeled `unpinned` because it does not expose the same affinity API.

### Requirements

- **CMake 3.16+**
- **C++17 compiler**
- **GoogleTest** when `ORDERBOOK_BUILD_TESTS=ON` (default)
- **Google Benchmark** when `ORDERBOOK_BUILD_BENCHMARKS=ON`

CMake searches for an installed Google Benchmark package and fetches version 1.9.4 when one is not available. GoogleTest is discovered through `find_package(GTest REQUIRED)`.

On macOS, GoogleTest can be installed with:

```bash
brew install googletest
```

### Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DORDERBOOK_BUILD_TESTS=ON -DORDERBOOK_BUILD_BENCHMARKS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The current suite contains **19 GoogleTest cases** covering:

- price-time priority, partial fills, and non-crossing orders;
- cancellation, invalid input, capacity, and order-slot reuse;
- price-block boundaries, best-price lookup, and empty-block cleanup;
- SPSC capacity, FIFO order, and two-thread transfer;
- complete request-to-event engine behavior;
- fixed-rate scheduling and deterministic benchmark validation.

### Run Benchmarks

Run each executable separately so benchmark groups do not compete for CPU:

```bash
./build/benchmarks/spsc_benchmarks
./build/benchmarks/orderbook_benchmarks
./build/benchmarks/engine_benchmarks
```

Run only one engine mode:

```bash
./build/benchmarks/engine_benchmarks --benchmark_filter=Throughput
./build/benchmarks/engine_benchmarks --benchmark_filter=OpenLoop
```

### Benchmark Environment Variables

| Variable | Purpose |
| --- | --- |
| `ORDERBOOK_BENCHMARK_REQUESTS` | Engine measured-request count; default 200,000, maximum 10,000,000, multiple of four |
| `ORDERBOOK_BENCHMARK_TARGET_RPS` | Explicit open-loop arrival rate; bypasses automatic calibration |
| `ORDERBOOK_BENCHMARK_PRODUCER_CPU` | Linux logical CPU for the producer |
| `ORDERBOOK_BENCHMARK_ENGINE_CPU` | Linux logical CPU for the matching engine |
| `ORDERBOOK_BENCHMARK_CONSUMER_CPU` | Linux logical CPU for the consumer |

Example fixed-rate run:

```bash
ORDERBOOK_BENCHMARK_REQUESTS=1000000 ORDERBOOK_BENCHMARK_TARGET_RPS=500000 ./build/benchmarks/engine_benchmarks --benchmark_filter='OpenLoop/50/' --benchmark_out=engine-open-loop.json --benchmark_out_format=json
```

---

## 0x03 Benchmarks

### Test Environment

Measurements below were collected on 2026-10-22:

| Component | Configuration |
| --- | --- |
| CPU | Apple M5 Pro, 15 cores |
| Memory | 24 GB |
| OS | macOS 26.7.1 |
| Compiler | Apple Clang 21.0.0 |
| Build | Release, `-O3 -DNDEBUG` |
| CPU binding | Unpinned |

Results are a reproducibility snapshot, not a latency SLA or a cross-machine comparison. macOS scheduling, background load, power state, and core placement can materially affect tail latency.

### 1. SPSC One-Item-in-Flight Handoff

Each payload performs 1,000,000 handoffs with 100,000 warmup items. One out of every 100 post-warmup handoffs is sampled, producing 9,000 latency samples. The producer waits for acknowledgment before sending the next sequence number, so this is a serialized handoff test rather than saturated queue throughput.

| Payload | Handoff Rate | P50 | P99 | Maximum | CV |
| --- | ---: | ---: | ---: | ---: | ---: |
| 16 bytes | 5.71M/s | 125 ns | 167 ns | 417 ns | 16.84% |
| 40 bytes | 5.80M/s | 166 ns | 209 ns | 5.58 us | 45.11% |

### 2. Core OrderBook Throughput

These tests call `OrderBook` directly without either SPSC channel. Each case contains 100,000 operations, including 10,000 warmup operations. Because individual operations can be shorter than the host clock tick, timing uses 16-operation batches and reports per-operation batch averages.

| Operation | Throughput | Batch P50 | Batch P99 | CV |
| --- | ---: | ---: | ---: | ---: |
| Resting add | 68.25M ops/s | 13 ns | 65 ns | 79.29% |
| Random cancellation | 25.14M ops/s | 39 ns | 75 ns | 40.30% |
| Single-maker match | 53.03M ops/s | 18 ns | 26 ns | 58.09% |
| Ten-maker sweep | 6.98M ops/s | 132 ns | 177 ns | 260.85% |

`Batch P50/P99` are distributions of batch averages, not request-level end-to-end percentiles. The large CV values reflect extremely short typical batches plus occasional operating-system outliers.

### 3. Complete Engine Pipeline

The complete benchmark runs separate producer, engine, and consumer threads through two 4,096-entry SPSC channels. It performs 20,000 warmup requests followed by 200,000 measured requests.

The deterministic four-request cycle is:

1. add a resting sell;
2. add a buy one tick below it;
3. add a matching buy at the sell price;
4. cancel the lower resting buy.

Each cycle produces four terminal acknowledgments and one trade event, then returns the book to empty.

The unpaced pipeline achieved:

| Metric | Result |
| --- | ---: |
| Median request throughput | **4.95M requests/s** |
| Median event throughput | **6.19M events/s** |
| Whole-run throughput CV | **0.72%** |
| Request size | 40 bytes |
| Event size | 64 bytes |

The unpaced producer intentionally saturates the bounded request queue. Failed enqueue attempts are counted as retries; requests are not dropped.

### 4. Open-Loop Scheduled Latency

Open-loop cases independently calibrate the unpaced pipeline and offer 50%, 70%, 85%, and 95% of the measured reference rate. Scheduled latency runs from the intended arrival time to the final acknowledgment.

| Load | Target | Achieved | P50 | P95 | P99 | P99.9 | Maximum | CV |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 50% | 2.48M req/s | 2.48M req/s | 420 ns | 624 ns | 13.40 us | 126.06 us | 134.26 us | 585.10% |
| 70% | 3.44M req/s | 3.44M req/s | 428 ns | 550 ns | 11.58 us | 159.40 us | 179.45 us | 708.37% |
| 85% | 4.22M req/s | 4.22M req/s | 445 ns | 566 ns | 6.86 us | 49.69 us | 63.07 us | 380.87% |
| 95% | 4.67M req/s | 4.67M req/s | 464 ns | 3.42 us | 16.36 us | 104.97 us | 112.82 us | 475.90% |

All four runs completed without request-channel-full retries or dropped requests. Producer lateness of at least one interarrival interval was 0.30%-0.70%, and the observed post-window drain was 0-136 ns.

The sub-microsecond center and the large CV are compatible: rare macOS scheduling pauses are tens or hundreds of times larger than a normal request. The non-monotonic P99 values also show why one short, unpinned run must not be treated as a production tail-latency guarantee.

### Open-Loop Timing Model

Arrival times are calculated from the original epoch:

```text
scheduled(index) = start + floor(index * 1,000,000,000 / requests_per_second)
```

Absolute deadlines prevent fractional nanosecond rounding from accumulating. When the producer is late or the request queue is full, the original schedule is retained so overload appears as lateness, backlog, and drain time.

For each sampled request:

```text
scheduled latency = submit lag + admission wait + admitted latency
```

The equality applies per request. Percentiles from the three component distributions must not be added together.

---

## 0x04 Project Layout

```text
include/orderbook/
├── engine.hpp                # request/event messages and engine loop
├── order_book.hpp            # order-book API and internal declarations
├── spsc_channel.hpp          # bounded SPSC ring and directional endpoints
└── types.hpp                 # shared scalar types

src/
└── order_book.cpp            # matching, cancellation, blocks, and pool

tests/
├── order_book/               # matching, cancellation, capacity, blocks
├── channel/                  # SPSC correctness and two-thread transfer
├── engine/                   # complete engine integration
└── benchmarks/               # schedule and workload validation

benchmarks/
├── common/                   # clock, affinity, and statistics helpers
├── channel/                  # standalone SPSC handoff
├── order_book/               # direct OrderBook throughput
└── engine/                   # throughput and open-loop pipeline latency
```

The standalone SPSC benchmark includes only the channel header and benchmark helpers. It does not link the order-book or matching-engine implementation.

---

## 0x05 Scope and Roadmap

### Implemented

- Limit-order add, crossing, and multi-order matching.
- Partial fills and FIFO priority.
- Cancellation and price-level aggregation.
- Fixed-capacity rejection and order-slot reuse.
- Typed request/event transport and ordered shutdown.
- Deterministic event validation during performance tests.

### Planned / Production Gaps

- Replace/amend, market, IOC, FOK, and post-only orders.
- Self-trade prevention.
- Fully allocation-free active-order indexing.
- Multi-symbol partitioning and sequenced multi-producer ingress.
- Network protocol parsing and kernel-bypass I/O.
- Persistence, replay, snapshots, and recovery.
- Reference-model differential tests, fuzzing, and longer tail-latency runs.
- Linux measurements with isolated cores, IRQ placement, and controlled CPU frequency.
- Production telemetry, overload policy, and fault handling.
