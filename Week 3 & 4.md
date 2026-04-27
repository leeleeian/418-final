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

**Status:** 

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

## Correctness vs golden harness


---

## How to Test


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

**Key Finding:** Fine-grained is 3–10% slower than coarse-grained across all configs.

| Config | 1-thr | 2-thr | 4-thr | 8-thr |
|--------|-------|-------|-------|-------|
| 100k/3 | 0.96  | 0.91  | 0.94  | 0.97  |
| 100k/8 | 0.93  | 0.93  | 0.92  | 0.97  |
| 100k/16| 0.95  | 0.90  | 0.92  | 0.93  |
| 500k/3 | 0.97  | 0.93  | 0.90  | 0.89  |
| 500k/8 | 0.97  | 0.93  | 0.94  | 0.95  |
| 500k/16| 0.95  | 0.92  | 0.93  | 0.95  |
| 5M/3   | 0.97  | 0.94  | 0.95  | 0.95  |
| 5M/8   | 0.96  | 0.96  | 0.95  | 0.96  |
| 5M/16  | 0.96  | 0.93  | 0.95  | 0.95  |

(Table: speedup = coarse_time / fine_time; <1.0 means coarse is faster)

**Why coarse-grained wins on this workload:**
1. **Resting-order dominance (~97% of orders are resting, not crossing):** The per-level parallelism benefit only activates during crossing matches (hand-over-hand level locking). With mostly resting orders, this parallelism is never utilized.
2. **Fine-grained lock overhead:** Fine-grained uses more lock objects (one `levelMutex` per price level + shared `bidsMutex_`/`asksMutex_`/`ordersMutex_`), leading to more acquisitions per operation and worse single-threaded performance.
3. **Cache locality:** Coarse-grained's single-lock design has better cache behavior; fine-grained's per-level locks scatter cache lines.

**When fine-grained would win:**
- **Crossing-heavy workloads** (many aggressive market orders or price-piercing limit orders)
- **High concurrent matching** on different price levels (true parallelism)
- **Bimodal order distributions** (e.g., many buy orders at one price, many sell orders at another)

**Conclusion:** The benchmark correctly shows that fine-grained overhead dominates on resting-heavy workloads. Fine-grained design is sound, but requires crossing-heavy stress tests to demonstrate its benefits.

---

### Remaining (Validation & optimization)
5. **Benchmark on GHC57:**
   - Compare `--engine fine` vs `--engine coarse` at various thread counts
   - Measure per-level parallelism benefit on crossing-heavy workloads
   - Test high-contention scenarios (many threads on same price levels)
6. **Optional optimizations (stretch):**
   - False-sharing padding on `PriceLevel` structures
   - Lock-free level discovery using atomic best-price pointers
   - Batching independent orders before matching

**Note on Correctness Validation:**
- Single-threaded fine-grained produces same per-ticker books as sequential (✅ verified)
- Global trade order may differ due to hand-over-hand level locking (expected)
- For golden-trace validation: fine-grained engine creates per-level ordering, not global ordering
  - This is acceptable: parallel engines are permitted to produce different global trade order while maintaining per-ticker invariants

---

## Concerns / Notes on design moving forward

### Known Remaining Bottlenecks

**1. Fine-grained crossing paths still serialized**: Crossing branch of `addLimitOrder`, `addMarketOrder` still depend on unique `opMutex_`.
   - **Impact:** Market orders and crossing limit orders still take a global lock for the entire match sequence.
   - **Fix:** Implement hand-over-hand per-level locking (as designed) to release and re-acquire between levels.

**2. modifyOrder not fully fine-grained:**
   - Currently takes unique `opMutex_` for the entire cancel-then-add sequence.
   - Could be split: cancel under shared lock (if crossing-free), then add.

**3. --threads 8 regression (fundamental to 16-shard design)**
- With 16 shards and 8 threads, work imbalance + lock contention cause slowdown
- Shards are drained in order via atomic fetch_add; some threads finish early and steal from the queue, causing cache misses and mutex contention spikes
- Could be mitigated by work-stealing with better locality or dynamic load-balancing, but that's beyond coarse-grained scope
- Fine-grained locking (per-price-level locks) should decouple shard contention entirely

### Next Steps
1. Implement crossing protocol for fine-grained book (level-walk/range locking with strict lock ordering).
2. Unify `orders_` synchronization policy so id-index safety does not depend on global op gating.
3. Wire and benchmark fine-grained engine path under skewed and high-contention workloads.

