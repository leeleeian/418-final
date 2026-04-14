# Milestone Report: Parallel Limit Order Book Simulation

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## Summary Of Work

We also completed a coarse-grained locking approach for our LOB. This involved two main components: the `CoarseGrainedLimitOrderBook`, which wraps the sequential LOB with one mutex per symbol, and the `CoarseGrainedMatchingEngine`, which adds a mutex around the lazy per-ticker book map. There is also parallel feeding of messages by ticker which preserves per-ticker arrival order and runs shards in parallel on pthreads. We ensured correctness of this implementation by getting a diff of its results with sequential golden.

---

## Progress w.r.t. Deliverables in Proposal


| Proposal item                                         | Status                                                                                               |
| ----------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| Correct single-threaded C++ LOB (price–time priority) | **Done** — `LimitOrderBook` + `MatchingEngine`; golden trace in `golden/`.                           |
| Controlled order generator + correctness harness      | **Done** — `OrderGenerator`, JSON dumps, `make baseline` / `make verify`.                            |
| Parallel LOB with coarse-grained locking              | **Done** — wrapper + multi-book engine + per-ticker parallel feed.                                   |
| Benchmarking / speedup scripts                        | **Done** — `scripts/bench_lob.sh` which is ran with `make bench`                                     |
| Throughput / latency as in proposal                   | **In progress** — throughput and end-to-end time are completed; per-message latency is not done yet. |
| Fine-grained locking on one book                      | **Not started** (Week 3+ per schedule).                                                              |
| Performance goal: >4× on 8 cores (coarse)             | **Not met yet** on the run below (~**1.5×** at 8 threads); see Preliminary Results and Concerns.     |


---

## Poster Session

**Figures we plan to show**

1. Speedup bar chart (primary) — Wall-time speedup vs the sequential baseline for coarse and fine parallel on 1, 2, 4, and 8 threads.
2. Order-book / fulfillment comparison — Final `books.json` from two runs: `diff` is clean for correct coarse vs sequential, so a poster table can highlight identical resting-order totals per ticker to show correctness.
3. Grouped / wall-time bars — panel that shows raw wall time (μs) or throughput (msgs/sec) as grouped bars by parallelism strategy for the same workload, to show the effects of lock overhead and/or shard parallelism.

---

## Preliminary Results

**Workload:** `seed=42`, `num-orders=500000` -> 500120 total messages, 3 tickers (AAPL / MSFT / GOOG), used GHC machine to run (specifically `ghc57`).

**Metric:** Wall-clock time printed by `./build/sim` in ms (message count kept constant). Speedup is calculated with respect to sequential baseline.


| Configuration                        | Time (μs) | Speedup vs sequential |
| ------------------------------------ | --------- | --------------------- |
| Sequential baseline                  | 175121    | 1.00x                 |
| Coarse LOB, sequential data          | 180604    | 0.97x                 |
| Coarse LOB, parallel data, N=1       | 153476    | 1.14x                 |
| Coarse LOB, parallel data, N=2       | 141364    | 1.24x                 |
| Coarse LOB, parallel data, N=4       | 113139    | 1.55x                 |
| Coarse LOB, parallel data, N=8       | 117055    | 1.50x                 |


**Throughput** (from same run, `msgs/sec` printed by sim): 2.86M -> 2.77M (coarse ST) -> up to 4.42M at 4 threads.

**Pthreads vs OpenMP** (same partition, matched thread count)  
On representative GHC runs before we removed the optional OpenMP build, the pthread worker pool consistently achieved marginally better wall time and throughput than an OpenMP `parallel for` over the same ticker shards. As a result, we standardized to always using pthreads for the coarse parallel path.

**Observations**

- **Coarse single-threaded** is slightly slower than sequential (~3%). This is because adding locks without actually parallelizing the feeding of messages leads to only overhead from having locks, and no added parallel benefits. 
- **Parallel by ticker** improves wall time up to 1.55x vs sequential at 4 threads; 8 threads does not beat 4 (1.50×), this is likely because we only use three  independent shards (only three tickers) so extra workers add contention or scheduling noise without more parallelism.
- `threads=1` parallel is still not “multicore parallel” since it uses the partition-and-shard driver with one worker. Any gain on sequential is likely due to better cache locality from processing one symbol at a time.

---

## Concerns

1. Proposal coarse goal (>4× on 8 cores) is not approached yet on this workload. To address this, we will test again with more shards/tickers.
2. Single-run timings are noisy, so we should use multiple trials and take the average result where possible

---

## Detailed Schedule

