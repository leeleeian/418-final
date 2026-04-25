# Milestone Report: Parallel Limit Order Book (LOB) Simulation

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---
Note: see Detailed Schedule at very bottom or on Home Page.

## Summary Of Work

From week 1, we built a correct, single-threaded baseline matching engine and the supporting infrastructure for benchmarking and regression testing. We established a core pipeline: **OrderGenerator → MatchingEngine → LimitOrderBook**. Shared primitive types (`Id`, `Price`, `Quantity`, `Side`) live in a single `Types.h` so all modules agree on widths. The `Order` class tracks resting state (initial/remaining quantity, fill status). The `LimitOrderBook` implements price-time priority matching using `std::map<Price, PriceLevel>` for O(log M) level access and `std::list<OrderPointer>` per level for FIFO ordering with O(1) cancel via an id-to-iterator map. The `OrderGenerator` produces a deterministic, seeded order stream (configurable mix of limit/market/cancel across multiple tickers) suitable for reproducible benchmarks. The `MatchingEngine` reveals `OrderMessage`s to per-ticker books, constructing `Order` objects on the new limit and stamping the ticker on each resulting `Trade`. On the default workload (seed=42, 50k orders, 3 tickers), the sequential baseline processes ~2.7M msgs/sec.

From week 2 and into 3, we also completed a parallel coarse-grained locking approach for our LOB. This involved two main components: the `CoarseGrainedLimitOrderBook`, which wraps the sequential LOB with one mutex per symbol, and the `CoarseGrainedMatchingEngine`, which adds a mutex around the lazy per-ticker book map. There is also parallel feeding of messages by ticker which preserves per-ticker arrival order and runs shards in parallel on pthreads. We ensured correctness of this implementation by getting a diff of its results with sequential golden.

Side note: The driver (`main.cpp`) wires everything together with CLI flags for seed, order count, and optional JSON dumps of the input stream, executed trades (grouped by ticker), and final book state (every resting order in price-time order). These dumps are deterministic for a given seed and stable across any implementation that preserves per-ticker arrival order which is a correctness invariant a parallel matcher must satisfy. A golden-trace script (`make baseline` / `make verify`) captures the sequential output and diffs against it in one command, giving us a complete check without writing explicit test cases.

---

## Progress w.r.t. Deliverables in Proposal

We believe that we are on track with our progress with respect to the deliverables we outlined in our proposal, given that we have successfully established the proper infrastructure, baseline, and testing to benchmark our later parallelism strategies for our LOB model. We will have some limited time to explore the nice-to-haves and reach goals including hybrid or lock-free approaches, and analyzing contention under skewed workloads. Although we likely would not have very performant results, it is within feasibility to adopt/ingest real data from low-latency environments from simulating to forecasting (a high reach goal detailed from our proposal). Specifically, here is our status updates on the work we have achieved and yet to achieve:

| Proposal item                                         | Status                                                                                               |
| ----------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| Correct single-threaded C++ LOB (price–time priority) | **Done** — `LimitOrderBook` + `MatchingEngine`; golden trace in `golden/`.                           |
| Controlled order generator + correctness harness      | **Done** — `OrderGenerator`, JSON dumps, `make baseline` / `make verify`.                            |
| Parallel LOB with coarse-grained locking              | **Done** — wrapper + multi-book engine + per-ticker parallel feed.                                   |
| Benchmarking / speedup scripts                        | **Done** — `scripts/bench_lob.sh` which is ran with `make bench`                                     |
| Throughput / latency as in proposal                   | **In progress** — throughput and end-to-end time are completed; per-message latency is not done yet. |
| Fine-grained locking on one book                      | **Not started** (Week 3+ per schedule).                                                              |
| Performance goal: >4× on 8 cores (coarse)             | **Not met yet** on the run below (1.44x at 8 threads); see Preliminary Results and Concerns.     |

More details on final work and reach goals can be found in our Detailed Schedule section.

---

## Poster Session

**Figures we plan to show**

1. Speedup bar chart (primary) — Wall-time speedup vs the sequential baseline for coarse and fine parallel on 1, 2, 4, and 8 threads (via GHC machines).
2. Order-book / fulfillment comparison — Final `books.json` from two runs at least, but more ideally highlighting difference between baseline, coarse, fine, and other extended implementations (across different thread counts): `diff` is clean for correct coarse vs sequential, so a poster table can highlight identical resting-order totals per ticker to show correctness, and ideally decrease in serial/overhead sections. This will likely look like the "Data, Synch, Busy" graphs that compare different assignment types that we have seen in lecture as thread count increases (against time).
3. Grouped / wall-time bars — panel that shows raw wall time (μs) or throughput (msgs/sec) as grouped bars by parallelism strategy for the same workload, to show the effects of lock overhead and/or shard parallelism.

**Figures nice to have**
Additionally, we might explore crossing rate analysis i.e. pie or bar chart showing what fraction of incoming orders cross (trigger a match) vs rest on the book. This might reveal how much of intra-book parallelism is actually in theory available for fine-grained locking. For example, if 45% of orders cross, then we might expect 45% of the operations to be inherently sequential under any locking scheme. Other visuals that we could explore (may or may not be valuable/informative) include lock contention heatmap, workload skew sensitivity, and latency distribution (if we add per-message timing).

---

## Preliminary Results

**Workload:** `seed=42`, `num-orders=500000` -> 500640 total messages, 16 tickers, used GHC machine to run (specifically `ghc57`).

**Metric:** Wall-clock time printed by `./build/sim` in ms (message count kept constant). Speedup is calculated with respect to sequential baseline.


| Configuration                        | Time (μs) | Speedup vs sequential |
| ------------------------------------ | --------- | --------------------- |
| Sequential baseline                  | 175121    | 1.00x                 |
| Coarse LOB, sequential data          | 180604    | 0.95x                 |
| Coarse LOB, parallel data, N=2       | 141364    | 1.77x                 |
| Coarse LOB, parallel data, N=4       | 113139    | 1.84x                 |
| Coarse LOB, parallel data, N=8       | 117055    | 1.44x                 |


**Throughput** (from same run, msgs/sec printed by sim): 2.61M (sequential baseline) -> 2.48M (coarse ST) -> 4.79M at 4 threads.

**Pthreads vs OpenMP** (same partition, matched thread count)  
On representative GHC runs before we removed the optional OpenMP build, the pthread worker pool consistently achieved marginally better wall time and throughput than an OpenMP parallel for over the same ticker shards. As a result, we standardized to always using pthreads for the coarse parallel path.

**Observations**

- **Coarse single-threaded** is slightly slower than sequential (~3%). This is because adding locks without actually parallelizing the feeding of messages leads to only overhead from having locks, and no added parallel benefits. 
- **Parallel by ticker** improves wall time up to 1.84x vs sequential at 4 threads; 8 threads does not beat 4 (1.44x), this could be because there is not enough work to saturate 8 threads so extra workers only add contention or scheduling noise without more parallelism.

---

## Concerns

1. Proposal coarse goal (>4× on 8 cores) is not approached yet on this workload. To address this, we will test again with more shards/tickers.
2. We identify 2 main bottlenecks:
   1. **Serial partitioning of loop**: in our Coarse Grained Matching Engine, we run single-threaded over every message before any worker starts i.e. 
   ```cpp
   for (const auto& m : msgs) {
    byTicker[m.ticker].push_back(m);   // hash + full OrderMessage copy (incl. std::string)
    }
   ```
   This means that for 500k messages, each iteration hashes the string, looks it up in
   the map, and creates a deep copy of an Order Message including the heap-allocated `std::string 
   ticker`. Consider Amdahl's Law: since partitioning took 35% of the total time, the maxmimum speedup is around 2.8x regardless of how many threads we use in this parallel section. Thus, this loop is one of the main reasons why we observe poor speedup, or even declining speedup at greater threads. 
   
   A proposed fix for this might be to build `std::vector<std::vector<std::size_t>>>` of indices into our original messages vector with no copies of Order Message at all, or to have each worker scan 1/N of the input and build local partitions, then merge. Note this is a consideration we take of adopting different parallelism paradigms i.e. data parallelism or model parallelism.

   2. **Per-message global mutex in `bookForMut`**: in the Coarse Grained Matching Engine, we acquire a `booksMapMutex_` once per message. Then we observe a following pipeline of calls: `drainShard` -> `onMessage` -> `bookForMut`, for every single message, even though every message in a shard has the same ticker. With N worker threads all calling `bookForMut` concurrently, this is a serious serialization point. This implies that all N threads queue on the same global lock 500k/N times each.

    Then, after partitioning, each shard has exactly one ticker. The book only needs to be locked up once per shard, not once per message. This could then be a cheaper fix and roughly worth 10-15% of overall computation time.

    A proposed fix could be to resolve the book pointer once we are at the start of `drainShard`, i.e. inline the dispatch loop to avoid touching `bookForMut` again. This would likely significantly drop global mutex acquisitions.

3. 8 threads regress relative to 4: With 16 tickers and 500k messages, each shard is around 31k messages. This means at 4 threads, each worker drains around 4 shards, whereas at 8 threads, each worker gets around 2 shards.. This means that the work per shard is small enough that the overhead of the work-stealing loop (atomic `fetch_add`), merge mutex, and L2 cache thrashing across 8 cores likely dominates the parallelism. With the parallel section of 500k messages only being around 110 ms (i.e. 14 ms per shard), there is not enough parallelism to amortize the thread overhead. This is a problem we actively would need to fix with fine-grained locking.
 
4. **Per-book mutex**: a natural implication/consequence of coarse grained locking is that each per-book mutex is uncontended but still costs some non-trivial ccomputation time. Since each shard is single-ticker and only one worker processes a given shard, the `CoarseGrainedLimitOrderBook::mutex_` is never actually contended in the parallel path. With atomic lock and unlock however, we see that `addLimitOrder` and `cancelOrder`, among others, will each have to pay a small amount of overhead. While this is negligible individually, with 500k messages roughly, this amounts to some pure mutex overhead.

Note that this is just what occurs by definition of coarse-grained locking, and not a significant issue: only contributing around 3% to computation time.

**How to move forward with fine-grained locking**: With fine-grained locking, we would replace the single per-book mutex with locks at smaller granularities inside the book. We would have to investigate the unit of locking though. We might consider the following strategies:

| Strategy | Parallelism within one book | Main risk |
| -------- | -------------------------- | --------- |
| Per-price-level lock | Buy at 100 and sell at 102 can proceed simultaneously if they don't cross | Matching requires crossing price levels i.e. a buy that fulfills out asks at 101, 102, 103 must lock them all or hold a range lock |
| Read-write lock on the whole book | Many concurrent reads (cancel lookups, snapshots), exclusive writes (matching) | If writes are frequent (~60% limits + 20% markets), the Read-Write lock effecitvley becomes just a regular mutex |
| Lock-free FIFO per level + Compare-and-Swap (CAS) on best-price pointer | Maximum concurrency for non-crossing limit orders (the common case) | Cancels need O(1) removal from the list, which is hard with lock-free structures. matching still needs some form of exclusion |


### Summary of Issues that concern us the most
1. **The serial partition is an Amdahl ceiling** since our data is structured such that messages arrive in a single interleaved stream and must be separated by ticker ebfore any parallelism can begin. Note that we can either parallelize the parititon itself i.e. each thread scans a disjoint chunk of the input or we could consider changing the architecture so that messages are never interleaved i.e. the generator emits per-ticker streams directly, which isn't an unrealistic representation.
2. **Fine-grained locking inside a single book is hard to implement** properly since we need to consider each step of the matching operation: filling orders, removing empty levels, walking from best to worst orders, etc which are all inherently sequential within a price-time priority book. For example, we can't necessarily match a buy at 101 and another buy at 102 truly concurrently since both would need to consume the same ask at 101. This means the degre of concurrency we can leverage depends on how often orders don't cross, but with realistic workloads with more dense spread at the midpoint, it is likley crossing occurs a lot.
3. There is a `std::string ticker` on every `OrderMessage` and `Trade` which means for every allocation, copy, and move, these heap-allocated strings impact the serial partition cost and every per-message path. In a production system, we might consider integer symbol IDs.

### Unknowns vs Known Work

| Work Type       |    Topic                                                                                                                    | Status                                                                                                     |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| **Known work** | Parallelize or eliminate the partition loop                                                                                    | Clear approach                                                                          |
| **Known work** | Cache book pointer in `drainShard`                                                                                            | Clear approach
| **Known work** | Increase workload size (5M+) for cleaner scaling curves                                                                       | Update data stream
| **Known work** | Implement per-price-level locking for non-crossing orders                                                                     | Exploratory locking (3 strategies to try)
| **Unknown**    | How much concurrency does per-level locking actually unlock on a realistic workload where most orders cluster near the spread? | Need to measure via profiling                       |
| **Unknown**    | Can matching i.e. the price-walking loop be made concurrent at all, or is it inherently serial?                                  | Likely a research question...      |

These two unknowns are the ones we'd highlight as research questions for the poster. Known work has a fairly clear path in terms of implementation. The partition fix and book-pointer caching should get us to 2.5–3×. Getting to 4×+ requires fine-grained locking that actually unlocks intra-book parallelism, which depends on the workload's crossing rate and measuring it.


---

## Detailed Schedule

✅ = done, ☑️ = in progress, ✔️ = planned

| Task | Who | Week 1 (03/24–03/31) | Week 2 (03/31–04/07) | Week 2.5 (04/07–04/14) | Week 3 (04/14–04/18) | Week 3.5 (04/18–04/22) | Week 4 (04/22–04/25) | Week 4.5 (04/25–04/28) | Week 5 (04/28–04/30) |
| ---- | ----- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- |
| Sequential LOB (`LimitOrderBook`, `Order`, `Types.h`) | Combined | ✅ | | | | | | | |
| Order generator (`OrderGenerator`, `GeneratorConfig`) | Lillian | ✅ | | | | | | | |
| Sequential `MatchingEngine` + `main.cpp` driver | Lillian | ✅ | | | | | | | |
| Golden-trace harness (`make baseline` / `make verify`) | Lillian | ✅ | | | | | | | |
| `CoarseGrainedLimitOrderBook` (mutex wrapper) | Irene | | ✅ | | | | | | |
| `CoarseGrainedMatchingEngine` + per-ticker pthread pool | Irene | | ✅ | | | | | | |
| Expand to 16 tickers, add `--engine` / `--parallel` / `--threads` CLI | Irene | | ✅ | | | | | | |
| `scripts/bench_lob.sh` + `make bench` | Combined| | ✅ | | | | | | |
| Milestone report, preliminary results on GHC | Combined | | | ✅ | | | | | |
| Bottleneck analysis (Amdahl partition, `bookForMut` mutex) | Combined | | | ✅ | | | | | |
| **Fix serial partition**: index-based sharding (no `OrderMessage` copies) | Lillian | | | | ✔️ | | | | |
| **Fix `drainShard`**: cache book pointer once per shard, inline dispatch | Irene | | | | ✔️ | | | | |
| Profile coarse benchmarks at 5M orders, verify 2.5–3× | Combined | | | | ✔️ | | | | |
| Write script to profile coarse benchmarks at 100k/500k/5M-message for 3/8/16 tickers | Irene | | | | ✔️ | | | | |
| Design fine-grained locking strategy (per-price-level vs RW lock vs CAS) | Combined | | | | ✔️ | | | | |
| Implement `FineGrainedLimitOrderBook`: per-level locks for non-crossing adds | Irene | | | | | ✔️ | | | |
| Implement fine-grained matching: range-lock or level-walk protocol for crossing orders | Lillian | | | | | ✔️ | | | |
| Fine-grained cancel: O(1) removal under level lock + global id-index lock | Irene | | | | | ✔️ | | | |
| `FineGrainedMatchingEngine` + wire into `main.cpp` (`--engine fine`) | Lillian | | | | | ✔️ | | | |
| Correctness: `make verify` for fine-grained path (same golden) | Combined | | | | | ✔️ | | | |
| Profile fine-grained on GHC: `perf stat`, cache misses, lock contention | Combined | | | | | | ✔️ | | |
| Pad data structures to avoid false sharing (cache-line alignment on `PriceLevel`) | Irene | | | | | | ✔️ | | |
| Evaluate under skewed workloads (e.g. 1 hot ticker + 15 cold) | Lillian | | | | | | ✔️ | | |
| Measure crossing rate vs non-crossing rate to explain intra-book parallelism | Combined | | | | | | ✔️ | | |
| [STRETCH] Batching: group independent non-crossing orders before matching | Combined | | | | | | | ✔️ | |
| [STRETCH] Lock-free FIFO per level + CAS on best-price pointer | Combined | | | | | | | ✔️ | |
| Final benchmarks: sweep 1/2/4/8 threads × sequential/coarse/fine × 500k/5M | Combined | | | | | | | ✔️ | |
| Generate speedup charts + throughput bar graphs for poster | Lillian | | | | | | | ✔️ | |
| Write final report | Combined | | | | | | | | ✔️ |
| Poster preparation + session | Combined | | | | | | | | ✔️ |

