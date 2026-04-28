# Week 5: Performance Analysis & Final Optimization

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## Goals

- Generate final benchmark sweep: sequential/coarse/fine × 1/2/4/8 threads × 500k/5M orders
- Produce visualization (speedup matrix, throughput charts) for poster
- Write final technical report summarizing findings

**Deadline:** April 30

**Status:** In progress

---

## Skewed Workload: Implementation & Results

### Feature: Hot Ticker Workload

**Problem:** Previous workloads (balanced/crossing/resting) distribute orders uniformly across all tickers. Real exchanges exhibit skewed traffic: 1-2 highly liquid instruments + many illiquid ones.

**Solution:** Added `--workload skewed` flag with `--skew-ratio` parameter to simulate realistic market conditions.

### Usage

**Command-line flags:**
```bash
--workload TYPE     balanced | crossing | resting | skewed (default balanced)
--skew-ratio RATIO  0-1: fraction of orders on first ticker (skewed only)
```

**Examples:**
```bash
# 90% orders on AAPL (first ticker), 10% split across rest
./build/sim --num-tickers 16 --workload skewed --skew-ratio 0.9

# More extreme: 95% on hot, 5% on cold
./build/sim --num-tickers 16 --workload skewed --skew-ratio 0.95

# Profile skewed across thread counts
./scripts/bench_lob.sh -workload skewed -grain fine
./scripts/bench_lob.sh -workload skewed -skew-ratio 0.95 -grain coarse
```

### Implementation Details

**OrderGenerator changes:**
- Added `GeneratorConfig.skewRatio` field
- Pre-compute CDF for weighted ticker selection in constructor
- `selectTickerIndex()` method uses cumulative distribution for O(ticker_count) selection

**Verified distribution (--skew-ratio 0.9, 16 tickers, 1000 orders):**
```
AAPL (hot):   959 orders (95.9% of 1000)
MSFT:          50 orders ( 5.0%)
GOOG:          49 orders ( 4.9%)
... (others): ~41-48 orders each
```

Book state reflects hot ticker concentration:
```
AAPL: 320 resting orders (vs 35-45 on cold tickers)
```

---

### Results: Skewed Workload Performance

#### Benchmark: Default Skew (0.9 = 90% hot)

**Coarse-grained (500k orders, 8 tickers, sequential baseline = 160,916 µs):**
```
Config                Wall Time    Speedup vs Sequential
sequential baseline   160,916 µs    1.00x
coarse ST             169,787 µs    0.95x
coarse 1-thread       157,039 µs    1.02x
coarse 2-thread       165,591 µs    0.97x
coarse 4-thread       162,487 µs    0.99x
coarse 8-thread       159,530 µs    1.01x
```

**Fine-grained (500k orders, 8 tickers, sequential baseline = 159,384 µs):**
```
Config                Wall Time    Speedup vs Sequential
sequential baseline   159,384 µs    1.00x
fine ST               177,009 µs    0.90x
fine 1-thread         166,764 µs    0.96x
fine 2-thread         178,886 µs    0.89x
fine 4-thread         175,262 µs    0.91x
fine 8-thread         175,270 µs    0.91x
```

**Speedup Comparison (coarse vs fine):**
```
Threads    Coarse    Fine     Speedup (coarse/fine)
1-thread   0.95x     0.90x    1.06x (coarse faster)
2-thread   0.97x     0.89x    1.09x (coarse faster)
4-thread   0.99x     0.91x    1.09x (coarse faster)
8-thread   1.01x     0.91x    1.11x (coarse faster)
```

#### Benchmark: Extreme Skew (0.95 = 95% hot)

**Fine-grained (500k orders, --skew-ratio 0.95):**
```
Config                Wall Time    Speedup
sequential baseline   157,502 µs    1.00x
fine ST               172,555 µs    0.91x
fine 1-thread         166,965 µs    0.94x
fine 2-thread         183,926 µs    0.86x
fine 4-thread         180,945 µs    0.87x
fine 8-thread         179,622 µs    0.88x
```

**Observation:** More extreme skew (95%) worsens fine-grained performance (0.88x vs 0.91x at 90% skew).

#### Comprehensive Comparison: All Workloads (Quick Mode)

**Format:** Speedup = coarse_time / fine_time  
**Interpretation:** >1.0 = fine is faster, <1.0 = coarse is faster

**Test Configuration:** 3 order counts (100k/500k/5M) × 8 tickers × 4 thread counts (1/2/4/8)

---

**Workload: BALANCED** (60% limit, 20% market, 20% cancel)
```
Config      1-thread   2-thread   4-thread   8-thread
────────────────────────────────────────────────────
100k/8        0.95       0.93       0.94       0.95
500k/8        0.96       0.90       0.92       0.95
5M/8          0.96       0.95       0.95       0.96

Average:      0.96       0.93       0.94       0.95
```

**Workload: CROSSING** (30% limit, 60% market, 10% cancel)
```
Config      1-thread   2-thread   4-thread   8-thread
────────────────────────────────────────────────────
100k/8        0.92       0.92       0.90       0.93
500k/8        0.91       0.90       0.93       0.93
5M/8          0.90       0.91       0.93       0.94

Average:      0.91       0.91       0.92       0.93
```

**Workload: RESTING** (70% limit, 10% market, 20% cancel)
```
Config      1-thread   2-thread   4-thread   8-thread
────────────────────────────────────────────────────
100k/8        0.95       0.93       0.96       0.98
500k/8        0.96       0.96       0.95       1.00
5M/8          0.97       0.97       0.97       0.97

Average:      0.96       0.95       0.96       0.98
```

**Workload: SKEWED** (60/20/20 mix + 90% orders on first ticker)
```
Config      1-thread   2-thread   4-thread   8-thread
────────────────────────────────────────────────────
100k/8        0.92       0.84       0.93       0.89
500k/8        0.96       0.92       0.94       0.91
5M/8          0.95       0.95       0.95       0.96

Average:      0.94       0.90       0.94       0.92
```

---

**Key Observations:**

| Workload | Avg Speedup | Worst Case | Best Case | Pattern |
|----------|-------------|-----------|-----------|---------|
| **Balanced** | 0.94x | 0.90x (2-thread) | 0.96x (1-thread) | Consistent coarse advantage |
| **Crossing** | 0.92x | 0.90x (100k/5M) | 0.94x (5M/8-thread) | Coarse wins most, especially on scaling |
| **Resting** | 0.96x | 0.95x (100k/2-thread) | 1.00x (500k/8-thread) | Closest competition, near parity at 8-thread |
| **Skewed** | 0.92x | 0.84x (100k/2-thread) | 0.96x (5M) | Hot ticker adds variance |

**Summary:** 
- **All workloads favor coarse-grained** (speedup < 1.0)
- **Crossing workload worst for fine** (0.91x avg) — heavy market orders trigger hand-over-hand 3N lock ops
- **Resting workload best for fine** (0.96x avg) — fewer matches mean fewer critical sections
- **Skewed workload variance** (0.84-0.96x) — hot ticker creates contention variance
- **No workload reveals fine-grained advantages** despite varied order mixes

---

### Analysis

**Why skewed doesn't hurt fine-grained more:**

1. **Hand-over-hand matching is workload-independent:** Per-level lock re-acquisition happens regardless of ticker skew
2. **Level locks are per-price, not per-ticker:** Even on hot ticker, level locks are still fine-grained (not all orders at same price)
3. **Cold tickers reduce contention slightly:** Fewer threads compete on same locks, but doesn't change fundamental 3N lock acquisitions per market order

**Implications:**
- Fine-grained locking loses on all workloads uniformly (~5-10% slower)
- No specific workload reveals advantages that might justify the complexity
- Coarse-grained is the right design for realistic order distributions

---

### Task 4: Final Benchmarks (Priority: High)
- [ ] Run full matrix sweep: seq/coarse/fine × 1/2/4/8 threads × 500k/5M orders
- [ ] Generate speedup tables and charts
- [ ] Analyze scaling characteristics

### Task 5: Visualization & Report (Priority: High)
- [ ] Create speedup matrix tables for poster
- [ ] Generate throughput comparison bar charts
- [ ] Write technical section for final report explaining findings
- [ ] Include lock hierarchy analysis and cache behavior

---

## Testing & Profiling: Updated Scripts

All profiling scripts now include the **skewed workload:**

### `./scripts/compare_grains_by_workload.sh`
Tests: balanced, crossing, resting, **skewed**

```bash
./scripts/compare_grains_by_workload.sh --quick
# Shows speedup table for all 4 workloads × 3 order counts × 4 thread counts
```

### `./scripts/bench_lob.sh`
Added `-workload skewed` and `-skew-ratio RATIO` options:

```bash
./scripts/bench_lob.sh -workload skewed -grain fine
./scripts/bench_lob.sh -workload skewed -skew-ratio 0.95 -grain coarse
```

### `./scripts/profile_engines_comprehensive.sh`
Automatically tests skewed workload in quick/full modes:

```bash
./scripts/profile_engines_comprehensive.sh --quick
# Profiles: balanced, crossing, resting, skewed
# Outputs perf stats (cycles, IPC, cache misses) to CSV
```

---

## Workload Summary

| Workload | Order Mix | Real-World Pattern | Lock Behavior |
|----------|-----------|-------------------|---------------|
| **balanced** | 60% limit, 20% market, 20% cancel | Default | Baseline contention |
| **crossing** | 30% limit, 60% market, 10% cancel | High liquidity | Heavy matching |
| **resting** | 70% limit, 10% market, 20% cancel | Liquidity provision | Order queue growth |
| **skewed** | 60/20/20 mix + hot ticker (90%+ orders) | Real markets | Concentrated lock contention |