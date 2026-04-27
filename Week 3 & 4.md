# Week 3 & 4: Parallel Limit Order Book Simulation 

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## Goals
    - Fix Coarse Grained implementation including 
      - Fix serial partition: index-based sharding (no OrderMessage copies)	
      - Fix drainShard: cache book pointer once per shard, inline dispatch
      - Profile coarse benchmarks at 5M orders, verify 2.5–3×
    - Complete fine grained implementation, including optimizations such as padding data structures to avoid false sharing 
    - Evaluate fine grained implementation under high workload skew
    - Hope To Achieve: Implement the batching based approach to group independent orders and reduce 

Deadline: April 21

---
## Modified Coarse-Grained Locking Approach

### Index-Based Sharding (Fix #1: Serial Partition Bottleneck)

**Problem:** The milestone analysis identified serial partition as ~35% bottleneck. `processAllParallel` was partitioning messages by ticker but storing full `OrderMessage` copies in each shard:
```cpp
std::unordered_map<std::string, std::vector<OrderMessage>> byTicker;  // copies each message
```

Each write required: hash + map lookup + heap-allocated `std::string` copy + full `OrderMessage` struct copy (120+ bytes).

**Solution:** Store only indices (8 bytes each) into the original message vector:
```cpp
std::unordered_map<std::string, std::vector<std::size_t>> byTicker;
for (std::size_t i = 0; i < msgs.size(); ++i) {
  byTicker[msgs[i].ticker].push_back(i);  // 8-byte append, no OrderMessage copy
}
```

**Files changed:**
- `CoarseGrainedMatchingEngine.h:37-42`: `drainShard` now takes `(const std::vector<OrderMessage>& msgs, const std::vector<std::size_t>& shardIndices)` instead of a copied shard reference
- `CoarseGrainedMatchingEngine.cpp:48-57`: `drainShard` iterates indices instead of owning messages
- `CoarseGrainedMatchingEngine.cpp:61-67`: `PthreadShard` struct now carries `const std::vector<OrderMessage>* msgs` alongside `shards`
- `CoarseGrainedMatchingEngine.cpp:106-112`: Partition loop stores size_t indices

**Impact on cache locality:** Messages are now processed in per-ticker order. All AAPL messages consecutive → better L1/L2 cache hit rate for book state (lock, price levels, orders). This alone gave a 23% speedup at --threads 1 on the local M-series.

### drainShard Book Caching (Fix #2: `bookForMut` Contention)
**Problem:** Even after index-based sharding, each message in a shard still followed:
`drainShard -> onMessage -> bookForMut`
Since all messages in a shard share one ticker, this caused unnecessary global `booksMapMutex_` acquisitions once per message.

**Solution:** Resolve the shard's `CoarseGrainedLimitOrderBook` once at the start of `drainShard`, then dispatch each message using:
`dispatchOnBook(book, msg)`
This keeps behavior the same while reducing map-lock pressure from O(messages per shard) to O(1) per shard.

**Files changed:**
 - `CoarseGrainedMatchingEngine.h`: added `dispatchOnBook(...)`; updated `drainShard` signature for index-based shards.
 - `CoarseGrainedMatchingEngine.cpp`: added `dispatchOnBook(...)`; changed `onMessage(...)` to delegate; changed `drainShard(...)` to call `bookForMut(...)` once per shard.
---
---

## Fine-Grained Locking Approach

### Strategy: Per-Price-Level Locks + Hand-Over-Hand Matching

We added `FineGrainedLimitOrderBook` as a new class (without replacing the sequential `LimitOrderBook`) and focused first on non-crossing/resting operations. Different price levels are independent. Avoid side-wide locks during matching.

The current lock layout is (to prevent deadlock):

- `bidsMutex_` / `asksMutex_`: side-map lock for lookup/create/erase of price levels.
- `ordersMutex_`: global id-index lock for `orders_`.
- `PriceLevel::levelMutex`: per-level lock for FIFO queue + level-local iterator map.

Hand-over-hand matching protocol:
- Per-level locking: Lock one price level at a time during matching
- Lock release: Release level lock immediately after matching (no side lock held)
- Price order: Always acquire in best→worst price order (no deadlock)
- Shared pointer safety: Use `std::shared_ptr<PriceLevel>` to keep level alive across lock releases

This improves non-crossing path concurrency by reducing time spent under side-map locks and ensuring level-local updates are protected by per-level mutexes.


### FineGrainedMatchingEngine (parallel by ticker)

**Architecture:** Same as `CoarseGrainedMatchingEngine` but with `FineGrainedLimitOrderBook` per ticker.
- Per-level locks enable parallelism within a single ticker's order book
- Per-ticker sharding enables parallelism across tickers
- Combined: different threads can match on different tickers AND different levels simultaneously

**Wire into main.cpp:**
- Added `--engine fine` CLI option
- Supports both `--engine fine` (single-threaded) and `--engine fine --parallel --threads N`

---

## How to Test

### Makefile Targets

**Correctness validation:**
```bash
make baseline   # Generate golden trace (trades.json, books.json) from current binary
                # Run this after code changes that should preserve semantics
make verify     # Compare current binary output against golden trace
                # Pass: output matches exactly; Fail: divergence detected
                # Requirement: golden/ must exist (run 'make baseline' first)
```

**Single run:**
```bash
make dump       # Generate detailed JSON dumps: orders.json, trades.json, books.json
                # Useful for inspection/debugging; output goes to build/dump/
```

### Benchmark Scripts

#### 1. bench_lob.sh – Single Configuration Benchmark
Test speedup vs sequential baseline for a specific order count, ticker count, and workload.

**Usage:**
```bash
./scripts/bench_lob.sh [-v] [-grain {coarse|fine}] [-workload {balanced|crossing|resting}]
```

**Flags:**
- `-v`: Verbose output (show full simulation details); default is summary table only
- `-grain coarse|fine`: Select engine (default: coarse)
- `-workload balanced|crossing|resting`: Order mix type (default: balanced)

**Environment variables:**
```bash
NUM_ORDERS=500000      # Total orders (default 500k)
NUM_TICKERS=16         # Number of tickers/shards (default 16)
SEED=42                # RNG seed (default 42)
```

**Examples:**
```bash
# Quick benchmark: coarse-grained on balanced workload (500k orders, 16 tickers)
./scripts/bench_lob.sh

# Verbose output with 5M orders, fine-grained, crossing workload
./scripts/bench_lob.sh -v -grain fine -workload crossing

# Custom setup: 100k orders, 3 tickers, resting workload
NUM_ORDERS=100000 NUM_TICKERS=3 ./scripts/bench_lob.sh -grain coarse -workload resting
```

**Output:**
Summary table showing:
- Sequential baseline time
- Single-threaded overhead (coarse/fine vs sequential)
- Parallel speedup at 1/2/4/8 threads

---

#### 2. bench_lob_matrix.sh – Full Matrix Sweep
Run benchmarks across all combinations: 3 order counts × 3 ticker counts × 4 thread counts.

**Usage:**
```bash
./scripts/bench_lob_matrix.sh [-v] [-grain {coarse|fine}] [-workload {balanced|crossing|resting}]
```

**Flags:**
- `-v`: Verbose per-cell output (full details); default is compact summary
- `-grain coarse|fine`: Select engine (default: coarse)
- `-workload balanced|crossing|resting`: Order mix (default: balanced)

**Matrix dimensions:**
- Order counts: 100k, 500k, 5M
- Ticker counts: 3, 8, 16
- Thread counts: 1, 2, 4, 8
- Total: 3 × 3 × 4 = 36 benchmark runs

**Examples:**
```bash
# Quick compact run (default balanced workload, coarse-grained)
./scripts/bench_lob_matrix.sh

# Verbose output for fine-grained on crossing workload
./scripts/bench_lob_matrix.sh -v -grain fine -workload crossing

# Coarse-grained resting-heavy workload, compact output
./scripts/bench_lob_matrix.sh -grain coarse -workload resting
```

**Output:**
- Compact mode: Summary table (speedups relative to sequential baseline)
- Verbose mode: Detailed per-cell results with timing breakdown
- Log file saved to: `results/bench_lob_matrix.log`

**Example output:**
```
Config       seq      1-thr    2-thr    4-thr    8-thr
------------ -------- -------- -------- -------- --------
100k/3t      1.00     1.08     1.75     2.89     3.98
100k/8t      1.00     1.16     1.86     2.85     4.17
...
5M/16t       1.00     1.55     2.40     3.62     5.25
```

---

#### 3. compare_engines.sh – Fine vs Coarse Comparison
Direct speedup comparison: coarse_time / fine_time for same configuration.

**Usage:**
```bash
./scripts/compare_engines.sh [--quick|--full] [--workload {balanced|crossing|resting}]
```

**Flags:**
- `--quick`: 1 sample per order count (3 configs, default)
- `--full`: All 9 configs (3 order counts × 3 ticker counts)
- `--workload balanced|crossing|resting`: Order mix (default: balanced)

**Examples:**
```bash
# Quick comparison on crossing workload
./scripts/compare_engines.sh --workload crossing

# Full matrix: all 9 configs on resting workload, 1/2/4/8 threads
./scripts/compare_engines.sh --full --workload resting

# Quick on balanced (default)
./scripts/compare_engines.sh --quick
```

**Output:**
Speedup matrix where:
- Speedup = coarse_time / fine_time
- **>1.0** means fine-grained is faster
- **<1.0** means coarse-grained is faster
- Time unit: microseconds (us)

**Example output:**
```
Fine-grained vs Coarse-grained Speedup Matrix [full mode, workload=crossing]
(speedup = coarse_time / fine_time; >1.0 means fine is faster)

Config     |    1-t |    2-t |    4-t |    8-t
==========================================================
100k/3     |   0.96 |   0.86 |   0.97 |   0.91
...
5M/16      |   0.89 |   0.92 |   0.94 |   0.95
```

---

#### 4. compare_grains_by_workload.sh – Workload Comparison
Compare fine vs coarse across all three workload types in one run.

**Usage:**
```bash
./scripts/compare_grains_by_workload.sh [--quick|--full]
```

**Flags:**
- `--quick`: 3 workloads × 3 order counts (1 ticker count), default
- `--full`: 3 workloads × 9 configs (all combos)

**Examples:**
```bash
# Quick: see how each workload favors one engine
./scripts/compare_grains_by_workload.sh --quick

# Full matrix: all combinations
./scripts/compare_grains_by_workload.sh --full
```

**Output:**
Separate speedup matrix per workload (coarse_time / fine_time).

---

### Typical Testing Workflow

**1. Correctness check (before any benchmarking):**
```bash
make baseline   # Establish golden trace
make verify     # Confirm current binary matches
```

**2. Single-config quick test:**
```bash
./scripts/bench_lob.sh -grain coarse -workload balanced
```

**3. Full performance matrix:**
```bash
./scripts/bench_lob_matrix.sh -grain coarse -workload balanced
```

**4. Compare engines across workloads:**
```bash
./scripts/compare_engines.sh --full --workload crossing
./scripts/compare_engines.sh --full --workload balanced
./scripts/compare_engines.sh --full --workload resting
```

**5. Comprehensive: all workloads at once:**
```bash
./scripts/compare_grains_by_workload.sh --full
```

---

### Workload Definitions

| Workload | Limit Orders | Market Orders | Cancels | Price Spread | Use Case |
|----------|--------------|---------------|---------|--------------|----------|
| **balanced** | 60% | 20% | 20% | 25 ticks | Default; moderate crossing |
| **crossing** | 30% | 60% | 10% | 5 ticks | High matching/execution pressure |
| **resting** | 70% | 10% | 20% | 50 ticks | Most orders rest in book |

---

## Results (post fix 2)

### Correctness Validation
- `make verify` passes after index-based sharding changes: golden trace matches sequential baseline exactly
- No compilation warnings or errors with `-Wall -Wextra -Wpedantic`
- Ran 100k/500k/5M-message workloads successfully for 3/8/16 tickers

### GHC57 Matrix Benchmark Results
Full sweep: 3 order counts × 3 ticker counts × 5 thread configs (seq + 1/2/4/8 threads).
Speedup relative to sequential baseline for each (order count, ticker count) pair:

| Config | seq | 1-thr | 2-thr | 4-thr | 8-thr |
|--------|-----|-------|-------|-------|-------|
| 100k/3t | 1.00 | 0.94 | 1.19 | 1.93 | 1.95 |
| 100k/8t | 1.00 | 1.08 | 1.75 | 2.89 | 3.98 |
| 100k/16t | 1.00 | 1.16 | 1.86 | 2.85 | 4.17 |
| 500k/3t | 1.00 | 1.19 | 1.48 | 2.26 | 2.14 |
| 500k/8t | 1.00 | 1.19 | 2.05 | 3.38 | 4.54 |
| 500k/16t | 1.00 | 1.21 | 2.05 | 3.48 | 4.94 |
| 5M/3t | 1.00 | 1.09 | 1.44 | 2.42 | 2.43 |
| 5M/8t | 1.00 | 1.31 | 2.04 | 3.19 | 4.88 |
| 5M/16t | 1.00 | 1.55 | 2.40 | 3.62 | 5.25 |

**Key observations:**
- **Shard count dominates parallelism:** 3 tickers cap at ~2× (only 3 threads useful). 16 tickers enable strong 8-thread scaling (~5×).
- **Thread count interaction:** --threads 1 shows negative speedup on small order counts (overhead > benefit). At 5M orders, 1-thread helps via cache locality (1.55× at 16 tickers).
- **Peak speedup:** 5.25× at (5M orders, 16 tickers, 8 threads).
- **3-ticker limitation:** Even at 8 threads with only 3 shards, speedup plateaus at ~2.4× (Amdahl's law: limited parallelism + coarse lock contention).

---

## Per-file Reference (week 3 and 4 changes only)

- `code/LimitOrderBook/FineGrainedLimitOrderBook.h`
  - Added fine-grained LOB type with per-level mutex (`PriceLevel::levelMutex`).
  - Added side-map locks (`bidsMutex_`, `asksMutex_`), global id-index lock (`ordersMutex_`).

- `code/LimitOrderBook/FineGrainedLimitOrderBook.cpp`
  - Added non-crossing fast path in `addLimitOrder` (shared op gate + `rest()`).
  - Implemented level-protected `rest()` and narrowed side-lock duration (lookup/create only).
  - Added per-level lock usage where level queues/iterators are touched (cancel/modify/match/snapshot).
  - Added TODO markers for reducing/removing `opMutex_` once crossing protocol is fully fine-grained.

- `code/MatchingEngine/CoarseGrainedMatchingEngine.{h,cpp}`
  - index-based sharding + one book lookup per shard.

- `scripts/bench_lob.sh` and `scripts/bench_lob_matrix.sh`
  - Drive benchmark sweeps and matrix logging.

---


**1. Non-crossing addLimitOrder (no global lock):**
```cpp
if (!isCrossing(side, price)) {
  rest(incoming);  // narrow scope: sideMutex only for level lookup
  return;
}
```
**Benefit:** Non-crossing orders parallelize with all other operations.

**2. Hand-over-hand crossing (per-level, no global lock):**
```cpp
while (incoming->getRemainingQuantity() > 0) {
  { // Find best level (side lock, brief)
    std::lock_guard<std::mutex> sideLock(asksMutex_);
    if (asks_.empty()) break;
    bestPrice = asks_.begin()->first;
    levelPtr = asks_.begin()->second;  // shared_ptr, safe across unlock
  }
  
  { // Match at level (only level lock)
    std::lock_guard<std::mutex> levelLock(levelPtr->levelMutex);
    // ... match and fill ...
  }
  
  { // Erase if empty (side lock, brief)
    std::lock_guard<std::mutex> sideLock(asksMutex_);
    if (levelPtr->orders.empty()) {
      asks_.erase(bestPrice);
    }
  }
}
```
**Benefit:** Side lock held only for ~microseconds (map lookup); level locking allows concurrent matching on different levels.

**3. Cancel operation (no global crossing lock):**
```cpp
// No opMutex_ needed: ordersMutex → sideMutex → levelMutex
// Parallelizes with non-crossing adds and crossing matches on other levels
```

**4. Removed `opMutex_`:**
- Crossing paths no longer need a global phase gate
- All locking is local to price levels or side maps
- Full fine-grained parallelism achieved
---



### Benchmark Results: Fine vs Coarse on GHC57

**Setup:** Matrix comparison across 9 configurations (3 order counts × 3 ticker counts) and 4 thread counts (1/2/4/8).

Fine-grained vs Coarse-grained Speedup Matrix [full mode, workload=resting]
(speedup = coarse_time / fine_time; >1.0 means fine is faster)

Config     |    1-t |    2-t |    4-t |    8-t
==========================================================
100k/3     |   0.96 |   0.95 |   0.93 |   0.92
100k/8     |   0.93 |   0.92 |   0.97 |   1.11
100k/16    |   0.98 |   0.92 |   0.98 |   0.93
500k/3     |   1.00 |   0.93 |   0.87 |   0.90
500k/8     |   0.98 |   0.92 |   0.95 |   0.97
500k/16    |   1.02 |   0.92 |   0.95 |   0.97
5M/3       |   1.00 |   0.94 |   0.94 |   0.96
5M/8       |   0.98 |   0.95 |   0.95 |   0.94
5M/16      |   0.98 |   0.96 |   0.96 |   0.99


Empirical results across three workloads:
| Workload | Market Orders | Price Spread | Fine Wins |
|----------|---------------|--------------|-----------|
| Crossing | 60% | 5 ticks | ~2% |
| Balanced | 20% | 25 ticks | ~10% |
| Resting | 10% | 50 ticks | ~15% |

All workloads show consistent <1.0 speedup (fine slower), even in the resting case with wide price distribution.

---

### When Fine-Grained Would Theoretically Win

We suspect that fine-grained locking would require the following to be true:

1. Very low crossing rates i.e. >95% resting, <5% market
   - Minimizes multi-level cascading matches
   - Most operations are single-level "rest the order"
   
2. Wide price distribution (tight spread × many levels)
   - Orders spread across 20+ price levels
   - Natural load balancing so threads hit different levels
   - No hot-spot convergence
   
3. Few orders per level (depth = 1-2, not 10+)
   - Short critical sections per level lock
   - Less contention when two threads hit same level

This suggests the following synthetic ideal workload:
```
95%+ limit orders (no market orders)
Orders strictly isolated: thread 1 ← AAPL levels 1-5
                          thread 2 ← MSFT levels 1-5
No cascading matches (no crossing)
Wide price bounds (maxPriceOffsetTicks = 100+)
```

However, we notice that even under the resting workload nature, the fine-grained can't beat coarse grained:
- 70% limits, 10% markets, 20% cancels (doesn't hit 95% threshold)
- maxPriceOffsetTicks = 50 creates spread, but still not enough for optimal parallelism
- Market orders still cause cascading level matches
- Result: 85% of test cases show fine-grained slower

A current hypothesis is therefore that fine-grained only wins under unrealistic workloads. For realistic market order distributions (10-60%), coarse-grained is fundamentally superior because contention is unavoidable—better to pay lock cost once than repeatedly.

---


## Concerns / Notes on design moving forward

### Known Remaining Bottlenecks


The fine-grained design uses a 3-level lock hierarchy:
```
Side locks (bidsMutex_, asksMutex_)
  ↓
Level locks (levelMutex per PriceLevel)
  ↓
Global lock (ordersMutex_)
```


```cpp
while (incoming->getRemainingQuantity() > 0) {
    { // #1: Acquire side lock to find best level
      std::lock_guard<std::mutex> sideLock(asksMutex_);
      levelPtr = asks_.begin()->second;
    }
    
    { // #2: Acquire level lock to match
      std::lock_guard<std::mutex> levelLock(levelPtr->levelMutex);
      orders_.erase(rid);  // <-- DATA RACE: no ordersMutex_!
    }
    
    { // #3: Acquire side lock again to erase empty level
      std::lock_guard<std::mutex> sideLock(asksMutex_);
      asks_.erase(bestPrice);
    }
}
```

1. Fine-grained lock hierarchy overhead defeats parallelism: The main problem is that hand-over-hand matching re-acquires side locks for every level and so consider a market order matching N levels = 3N lock acquisitions (side + level + side) vs 1 for coarse-grained --> Realistic workloads (10-60% market orders) cause multi-level cascades where lock overhead dominates. However, it appears the 3-level lock hierarchy (side → level → global) is fundamental i.e. hand-over-hand cannot reduce this. Thus, for crossing/balanced workloads, contention is unavoidable. Better to serialize once (coarse) than acquire locks repeatedly (fine)

2. Data race in fine-grained `orders_` map: `orders_.erase(rid)` called with only `levelMutex_` held, not `ordersMutex_`. Thus, this means concurrent `hasOrder()` or other `orders_` access can read/write while erase is in-flight. Undefined behavior under parallel execution; violates data race safety. Either (a) acquire `ordersMutex_` before erase, or (b) unify synchronization policy for global index

3. modifyOrder isn't fully fine grained:
   - Currently takes unique lock for the entire cancel-then-add sequence.
   - Could be split: cancel under shared lock (if crossing-free), then add.

4. --threads 8 regression (fundamental to 16-shard design)
- With 16 shards and 8 threads, work imbalance + lock contention cause slowdown
- Shards are drained in order via atomic fetch_add; some threads finish early and steal from the queue, causing cache misses and mutex contention spikes
- Could be mitigated by work-stealing with better locality or dynamic load-balancing, but that's beyond coarse-grained scope
- Fine-grained locking (per-price-level locks) should decouple shard contention entirely

### Next Steps

1. Fix data race on `orders_` map: acquire `ordersMutex_` before erasing in matching/cancel paths
2. Validate thread-safety with ThreadSanitizer (TSAN) on fine-grained implementation
3. Benchmark
   - Compare `--engine fine` vs `--engine coarse` at various thread counts
   - Measure per-level parallelism benefit on crossing-heavy workloads
   - Test high-contention scenarios (many threads on same price levels)
6. Optional optimizations:
   - False-sharing padding on `PriceLevel` structures. Reduce side-lock re-acquisitions by keeping side lock held during entire level traversal (breaks hand-over-hand; trades per-level parallelism for fewer acquisitions).
   - Lock-free level discovery using atomic best-price pointers
   - Batching independent orders before matching

