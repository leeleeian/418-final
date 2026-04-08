# Week 2: Parallel Limit Order Book Simulation 

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## Goals 

- Implement a parallel LOB with coarse grained locking
- Set up scripts for benchmarking that measure throughput and latency, as well as speedup/scalability.

Deadline: April 7th

**Status:** Coarse-grained LOB + matching engine + per-ticker parallel driver are implemented; `scripts/bench_lob.sh` / `make bench` report throughput (wall-clock `msgs/sec` from `main`). There is currently no script for benchmarking latency (Per-message) because that would require extra timing code inside the hot path, which would slow down the overall computation, or a separate microbenchmark.

---

## Coarse-Grained Locking Approach

The  `CoarseGrainedLimitOrderBook` adds a wrapper around the sequential `LimitOrderBook` that holds one `std::mutex` and forwards every public call to the inner book under `std::lock_guard`. This locks at the single-symbol book level so all inserts, cancels, and matches for one ticker serialize on one lock.

The multi-ticker `CoarseGrainedMatchingEngine` adds a second mutex around the `unordered_map` of books (`booksMapMutex_`). This mutex, `bookForMut`, only locks the map during the finding or emplacing of a `unique_ptr<CoarseGrainedLimitOrderBook>`. The actual matching then uses the per-book mutex inside the wrapper. This means that different threads touching different tickers therefore hold different book locks and can run concurrently.

The message feed has also now been made parallel (`processAllParallel`). Messages are partitioned by ticker, but within each ticker the relative order of messages is unchanged (compared to global stream). Each partition is processed on a separate pthread pool. The worker count is `min(requested threads, number of non-empty ticker shards)`, so with three tickers you cannot use more than three workers for this partitioning strategy. The in-memory vector of trades returned by `processAllParallel` is built by concatenating results of each thread pool in thread-completion order, meaning the order overall may be different from `MatchingEngine::processAll`, however the per ticker orders should be the same. The `dumpTradesJson` writer buckets by ticker, which would have a deterministic layout that matches the baseline workload for correctness checking.

---

## Correctness vs golden harness

- **`make verify`**: runs `./build/sim` with **default** options (`--engine sequential`), same as Week 1 — unchanged golden files.
- **`--engine coarse` without `--parallel`**: feeds messages in global order through the mutex-wrapped books. output matches the sequential golden (`trades.json` and `books.json` for the baseline seed/count).
- **`--engine coarse --parallel`:** `books.json` and `trades.json` match golden for the same workload when dumped from `main` (uses bucketed JSON). only the raw in-process trade vector ordering differs from sequential `processAll`.

---

## How to Test

From the repo root:

- `make` (or `make build`): compiles everything into `build/sim`, incremental -- only rebuilds what changed.
- `make run`: build (if needed) with default config (no dumps).
- `make dump`: build + run, writes to `build/dump/{orders, trades, books}.json`.
- `make baseline` / `make verify`: unchanged/unused in week 2 -- sequential engine only
- `make bench` (**NEW**): runs `scripts/bench_lob.sh` (sequential vs coarse ST vs coarse parallel for several `--threads` values). Override workload with e.g. `NUM_ORDERS=200000 SEED=42 make bench`.
- `make clean`: delete `build/` directory, doesn't touch `golden/`


**CLI flags on `./build/sim`** (in addition to Week 1):

| Flag | Default | Meaning |
| ---- | ------- | ------- |
| `--engine NAME` | `sequential` | `sequential` → `MatchingEngine`; `coarse` → `CoarseGrainedMatchingEngine` |
| `--parallel` | off | Per-ticker parallel feed (**requires** `--engine coarse`) |
| `--threads N` | `0` | Worker threads for `--parallel` (`0` = `std::thread::hardware_concurrency()`, minimum 1, capped by ticker shard count) |

`--parallel` without `--engine coarse` is rejected with an error.

---

## Preliminary Results 

TODO: Fill in after running `make bench` on GHC machines. Want table of `msgs/sec` vs thread count. Note on whether speedup plateaus at the number of tickers.

---

## Per-file Reference

Files are listed in dependency order. Week 2 adds layers beside the Week 1 stack (nothing overwrites `LimitOrderBook` or `MatchingEngine`).

`Types → Order → LimitOrderBook → CoarseGrainedLimitOrderBook → OrderGenerator → MatchingEngine | CoarseGrainedMatchingEngine → main`.

---

### `code/LimitOrderBook/CoarseGrainedLimitOrderBook.{h,cpp}`

Thread-safe wrapper around the existing single-threaded `LimitOrderBook`. One mutex serializes all access to the embedded book; the sequential implementation is reused with no copy of matching logic.

```cpp
class CoarseGrainedLimitOrderBook {
public:
  std::vector<Trade> addLimitOrder(Id id, Side side, Price price, Quantity quantity);
  std::vector<Trade> addMarketOrder(Id id, Side side, Quantity quantity);
  void cancelOrder(Id id);
  std::vector<Trade> modifyOrder(Id id, Price newPrice, Quantity newQuantity);

  bool hasOrder(Id id) const;
  std::size_t restingOrderCount() const;
  std::size_t bidLevelCount() const;
  std::size_t askLevelCount() const;
  BookSnapshot snapshot() const;

private:
  mutable std::mutex mutex_;
  LimitOrderBook book_;
};
```

---

### `code/MatchingEngine/CoarseGrainedMatchingEngine.{h,cpp}`

Same dispatch rules as `MatchingEngine` (`onMessage` stamps `Trade::ticker`), but each ticker’s book is a `CoarseGrainedLimitOrderBook`. The map of tickers → books is protected by `booksMapMutex_` so lazy insertion is safe under concurrency.

```cpp
class CoarseGrainedMatchingEngine {
public:
  std::vector<Trade> onMessage(const OrderMessage& msg);
  std::vector<Trade> processAll(const std::vector<OrderMessage>& msgs);

  // numThreads == 0 → hardware_concurrency(), at least 1, capped by shard count
  std::vector<Trade> processAllParallel(const std::vector<OrderMessage>& msgs,
                                      std::size_t numThreads = 0);

  const CoarseGrainedLimitOrderBook* bookFor(const std::string& ticker) const;

private:
  CoarseGrainedLimitOrderBook& bookForMut(const std::string& ticker);

  mutable std::mutex booksMapMutex_;
  std::unordered_map<std::string, std::unique_ptr<CoarseGrainedLimitOrderBook>> books_;
};
```

**`processAll`** loops in global message order (same as sequential `MatchingEngine::processAll`, but with additional lock overhead). 

**`processAllParallel`** builds per-ticker vectors, then workers pull shard indices from an atomic counter and append local trades into a shared result vector under a merge mutex.

---

### `code/main.cpp` (updates)

- **`EngineKind`:** `SEQUENTIAL` vs `COARSE`; drives which engine type is instantiated.
- **`CliOptions`:** adds `engine`, `parallel`, `threads`.
- **`dumpBooksJson`:** now a template so the same JSON writer works for both `MatchingEngine` and `CoarseGrainedMatchingEngine` (anything with `bookFor` → pointer-like object exposing `snapshot()`, `restingOrderCount()`, etc.).
- **`printBookSummary`:** templated the same way.
- **Flow:** If `engine == SEQUENTIAL`, behavior matches Week 1 (early return after dumps). If `COARSE`, uses `CoarseGrainedMatchingEngine` with either `processAll` or `processAllParallel`, prints engine label and optional thread budget, then the same trade/book reporting and dumps.

**Extended CLI table** (full set):

| Flag | Default | Meaning |
| ---- | ------- | ------- |
| `--seed N` | `42` | RNG seed |
| `--num-orders N` | `50000` | Main stream size |
| `--engine NAME` | `sequential` | `sequential` \| `coarse` |
| `--parallel` | off | Per-ticker parallel (coarse only) |
| `--threads N` | `0` | Parallel worker count (`0` = hardware default) |
| `--dump-orders PATH` | – | Input stream JSON |
| `--dump-trades PATH` | – | Trades JSON |
| `--dump-books PATH` | – | Final books JSON |
| `-h`, `--help` | – | Usage |

---

### `scripts/bench_lob.sh`

Shell driver for throughput sweeps: runs `build/sim` with baseline-style defaults, then coarse single-threaded, then coarse parallel with `--threads` in `1 2 4 8`. Environment variables `SEED` and `NUM_ORDERS` adjust the workload without editing the script.

---

### `Makefile` (updates)

- **`.PHONY`:** added `bench`.
- **`bench` target:** invokes `./scripts/bench_lob.sh`.

---

## Concerns / Notes on design moving forward

- **Partitioning by ticker** avoids cross-ticker races but caps useful parallelism at the number of symbols with traffic. A single hot ticker would need a different strategy (e.g. single global order with one worker, or future fine-grained locking inside one book).
- **`make verify`** uses the sequential engine only; coarse / parallel paths are checked separately with `diff` on dumps when needed.
- **Latency percentiles** need explicit instrumentation (per-message timers or external harness), not just end-to-end wall time.
- Golden harness still cannot catch “lucky” races; stress with repeated runs or ThreadSanitizer remains useful for later lock-free / fine-grained work.
