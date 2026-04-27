# Week 4: Performance Analysis & Final Optimization

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## Goals

- Profile fine-grained locking under realistic workloads (crossing/balanced/resting)
- Fix identified correctness issues (data race on `orders_` map)
- Implement performance optimizations (false-sharing padding, lock-free improvements)

**Deadline:** April 28

**Status:** In progress

---

## Performance Analysis: Initial Results

### Perf Stat Profile (Crossing Workload, 8 Threads)

**Test Configuration:**
```bash
perf stat -e cycles,instructions,cache-misses,L1-dcache-load-misses \
  ./build/sim --engine {fine|coarse} --num-orders 1000000 --num-tickers 8 \
  --parallel --threads 8 --workload crossing
```

#### Fine-Grained Engine Results:
```
Processed in 60650 us (~16,493,322 msgs/sec)
Trades produced: 549,535

Performance counter stats:
  1,495,248,677  cycles                                    
  2,592,902,524  instructions            #  1.73 insn per cycle
     12,962,693  cache-misses                              
     14,919,583  L1-dcache-load-misses                     

  0.201856 seconds time elapsed (wall time)
  0.336337 seconds user time
  0.048615 seconds sys time
```

#### Coarse-Grained Engine Results:
```
Processed in 58677 us (~17,047,906 msgs/sec)
Trades produced: 549,535

Performance counter stats:
  1,403,503,792  cycles                                    
  2,433,121,866  instructions            #  1.73 insn per cycle
     13,057,629  cache-misses                              
     14,895,499  L1-dcache-load-misses                     

  0.157232 seconds time elapsed (wall time)
  0.266671 seconds user time
  0.054727 seconds sys time
```

#### Analysis:

| Metric | Fine | Coarse | Difference |
|--------|------|--------|-----------|
| Wall Time (us) | 60,650 | 58,677 | +3.4% slower |
| Cycles | 1,495M | 1,403M | +6.6% more cycles |
| Instructions | 2,592M | 2,433M | +6.5% more instructions |
| IPC (Insn/Cycle) | 1.73 | 1.73 | Same |
| L1 dcache misses | 14.9M | 14.9M | Identical |
| Cache miss rate | ~0.87% | ~0.87% | Identical |

**Key Finding:** L1 cache behavior is **identical** between fine and coarse, suggesting the 3.4% slowdown is **not from cache misses but from lock overhead itself** (hand-over-hand re-acquisitions).

---

## Identified Issues & Fixes

1) The global `orders_` map is accessed without `ordersMutex_` protection while another thread may call `hasOrder()` or `modifyOrder()`, causing undefined behavior. Thus, we fix by acquire `ordersMutex_` before erase.

2) [TODO] modifyOrder Not Fully Fine-Grained: currently entire cancel-then-add sequence holds a single lock. We could try to split into phases: first, cancel under side + level lock (if crossing-free), then, add under separate locks

---

## Optimization Attempts

#1: [TODO] False-Sharing Padding (PriceLevel). The hypothesis is that multiple `PriceLevel` objects on the same cache line (64 bytes) cause false sharing when threads access different levels. The goal is to reduce cache-line conflicts between level locks.

**Implementation:**
```cpp
struct PriceLevel {
  Price price;
  std::list<OrderPointer> orders;
  std::unordered_map<Id, std::list<OrderPointer>::iterator> orderIters;
  alignas(64) mutable std::mutex levelMutex;  // Force cache-line alignment
};
```

---

#2: [TODO] Lock-Free Best-Price Pointer: acquiring side lock just to find best level is wasteful on hot path. We will want to use atomic compare-and-swap (CAS) on best-price pointer to aim to remove side lock from level discovery, reduce acquisitions from 3N to N.

```cpp
std::atomic<Price> bestAskPrice_{std::numeric_limits<Price>::max()};
```

---

## Planned Week 5 Tasks

### Task 1: Fix Data Race (Priority: Critical)
- [ ] Acquire `ordersMutex_` in matching paths before `orders_.erase()`
- [ ] Run `make verify` to confirm correctness
- [ ] Run ThreadSanitizer (TSAN) to validate thread-safety

### Task 2: Profile & Benchmark (Priority: High)
- [ ] Run `perf stat` across all workloads (crossing/balanced/resting)
- [ ] Measure lock contention with `strace -e futex`
- [ ] Generate speedup matrix: fine vs coarse at 1/2/4/8 threads
- [ ] Compare across workload types

### Task 3: Optimization Experiments (Priority: Medium)
- [ ] Implement false-sharing padding on `PriceLevel`
- [ ] Benchmark before/after padding
- [ ] Test if padding improves or makes no difference

---

## Critical Path Dependencies

1. **Fix data race** (blocker for TSAN validation)
2. **Run profiling** (informs optimization prioritization)
3. **Optimization experiments** (low ROI, quick to test)
4. **Final benchmarks** (feed into visualization)
5. **Report + poster** (uses benchmark data)

---

## Stretch Goals (If Time)

- [ ] Lock-free best-price pointer (CAS-based level discovery)
- [ ] Batching framework (group independent orders before matching)
- [ ] Dynamic workload detection (auto-switch engine based on crossing rate)
