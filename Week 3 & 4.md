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



---

## Correctness vs golden harness


---

## How to Test


---

## Results (post fix 2)

### Correctness Validation
- `make verify` passes after index-based sharding changes: golden trace matches sequential baseline exactly
- No compilation warnings or errors with `-Wall -Wextra -Wpedantic`
- Ran 500k-message and 5M-message workloads successfully

### Local M-Series Benchmarks (500k orders / 16 tickers)
Index-based sharding reduces partition overhead, enabling better cache locality. New drain shard implementation reduces unnecessary lock acquisition which in turn reduces overhead and enables more parallelism.

| Configuration | Time (ms) | Speedup vs seq | Notes |
|---|---|---|---|
| Sequential baseline | 122.437 | 1.00× | — |
| Coarse ST (no parallel feed) | 112.891 | 1.08× | slight improvement over baseline |
| Coarse parallel, --threads 2 | 79.759 | 1.54× | clear speedup from per-ticker parallelism |
| Coarse parallel, --threads 4 | 59.977 | 2.04× | strong scaling at mid thread count |
| Coarse parallel, --threads 8 | 37.681 | **3.25×** | best observed local speedup |

**Key insight:** On local M-series, throughput scales steadily from 2→4→8 threads, with best speedup at `--threads 8` (3.25× vs sequential).

### GHC57 Benchmarks (500k orders / 16 tickers)
Canonical hardware (Intel, 8 cores):

| Configuration | Time (ms) | Speedup vs seq |
|---|---|---|
| Sequential baseline | 187.436 | 1.00× |
| Coarse ST | 195.990 | 0.96× |
| Coarse parallel, --threads 2 | 91.319 | 2.05× |
| Coarse parallel, --threads 4 | 53.850 | 3.48× |
| Coarse parallel, --threads 8 | 37.318 | **5.02×** |

**Observations:**
- GHC57 shows stronger scaling than local M-series at all parallel thread counts.
- Peak measured speedup on GHC57 is 5.02× at `--threads 8`.
- Coarse ST remains slightly below sequential on GHC57, but parallel feed dominates overall runtime once threads are enabled.

---

## Per-file Reference


---


---

## Concerns / Notes on design moving forward

### Known Remaining Bottlenecks

**1. `bookForMut` per-message global mutex (Fix #2, scheduled)**
- `drainShard` calls `onMessage` for each message, which calls `bookForMut` to lock the `books_` map
- With 500k messages and 16 shards, that's 31k–62k lock acquisitions per thread depending on load balance
- On GHC57 with 8 threads, this mutex becomes the limiting factor → 8-thread regression from 1.86× (4-thread peak) to 1.41×
- **Fix:** Cache the book pointer once per shard; all messages in a shard access the same ticker, so one acquisition per shard instead of per message
- Expected gain: +10–15% based on Amdahl's law analysis

**2. --threads 8 regression (fundamental to 16-shard design)**
- With 16 shards and 8 threads, work imbalance + lock contention cause slowdown
- Shards are drained in order via atomic fetch_add; some threads finish early and steal from the queue, causing cache misses and mutex contention spikes
- Could be mitigated by work-stealing with better locality or dynamic load-balancing, but that's beyond coarse-grained scope
- Fine-grained locking (per-price-level locks) should decouple shard contention entirely

### Next Steps
1. Implement Fix #2: cache book pointer in `drainShard`
2. Run full test matrix on GHC57 (scale sweep 100k/500k/5M, ticker count sweep 3/8/16)
3. Move to fine-grained locking implementation and evaluate under high-contention scenarios (skewed workloads)

