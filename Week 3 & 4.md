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

We added `FineGrainedLimitOrderBook` as a new class (without replacing the sequential `LimitOrderBook`) and focused first on non-crossing/resting operations.

The current lock layout is:

- `opMutex_` (`std::shared_mutex`): temporary gate used in crossing paths at the moment
- `bidsMutex_` / `asksMutex_`: side-map lock for lookup/create/erase of price levels.
- `ordersMutex_`: global id-index lock for `orders_`.
- `PriceLevel::levelMutex`: per-level lock for FIFO queue + level-local iterator map.

For non-crossing `addLimitOrder`, the flow is:

1. shared `opMutex_` (can likely remove w/ updated crossing operations),
2. top-of-book crossing check (`isCrossing`),
3. `rest()` with narrow lock scopes:
   - side lock for level lookup/create,
   - per-level lock for queue append / iterator insertion,
   - id-index lock to publish into `orders_`.

This improves non-crossing path concurrency by reducing time spent under side-map locks and ensuring level-local updates are protected by per-level mutexes.

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
  - Added side-map locks (`bidsMutex_`, `asksMutex_`), global id-index lock (`ordersMutex_`), and phase gate (`opMutex_`).

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


---

## Concerns / Notes on design moving forward

### Known Remaining Bottlenecks

**1. Fine-grained crossing paths still serialized**: Crossing branch of `addLimitOrder`, `addMarketOrder`, and crossing-heavy portions of `modifyOrder` still depend on unique `opMutex_`.

**2. Mixed lock policy around `orders_` during crossing loops**
- Non-crossing/cancel paths use explicit `ordersMutex_`, but crossing loops still rely on unique `opMutex_` for id-index safety.
- Correct rigth now, but lock policy should be unified before fully removing/reducing op-level gating.

**3. --threads 8 regression (fundamental to 16-shard design)**
- With 16 shards and 8 threads, work imbalance + lock contention cause slowdown
- Shards are drained in order via atomic fetch_add; some threads finish early and steal from the queue, causing cache misses and mutex contention spikes
- Could be mitigated by work-stealing with better locality or dynamic load-balancing, but that's beyond coarse-grained scope
- Fine-grained locking (per-price-level locks) should decouple shard contention entirely

### Next Steps
1. Implement crossing protocol for fine-grained book (level-walk/range locking with strict lock ordering).
2. Unify `orders_` synchronization policy so id-index safety does not depend on global op gating.
3. Wire and benchmark fine-grained engine path under skewed and high-contention workloads.

