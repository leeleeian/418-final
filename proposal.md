# Parallel Limit Order Book Simulation – Proposal

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**CS 418 – Spring 2026**

🔙 [Back to Main Page](index.html)

---

## Summary

We propose to build a parallel simulation of a limit order book (LOB) to study how different synchronization strategies impact scalability under realistic financial workloads. Our implementation will explore multi-core CPU parallelism using threading and synchronization primitives.

---

## Background

A limit order book is the core data structure used by financial exchanges to match buy and sell orders. Orders are processed according to price-time priority, meaning that better-priced orders are matched first, and among equal prices, earlier orders take precedence.

The system maintains two ordered structures (bids and asks), and operations include order insertion, cancellation, and matching. These operations are highly interdependent, as each update modifies global state and affects subsequent matching behavior.

While parts of the system—such as order generation—are naturally parallelizable, the matching engine introduces strong dependencies that limit parallelism. This makes the LOB a compelling case study for understanding the tradeoffs between concurrency and correctness.

---

## The Challenge

The primary challenge lies in maintaining correctness while introducing parallelism. The workload exhibits:

- **Strong dependencies**: order matching depends on global state and strict ordering
- **High contention**: most activity occurs near the best bid/ask
- **Irregular memory access patterns**: pointer-heavy structures and dynamic updates
- **Low compute-to-synchronization ratio**: operations are small but require coordination

Naive parallelization strategies such as coarse-grained locking severely limit scalability, while fine-grained locking and lock-free data structures introduce overhead from synchronization, retries, and cache coherence effects.

We aim to explore how restructuring computation—rather than fully parallelizing it—can improve performance.

---

## Resources

We will implement the system in C++ using multithreading (pthreads or std::thread).

We will run experiments on:
- Multi-core CPUs available on CMU lab machines
- Possibly PSC clusters if needed

We will build the system from scratch but reference:
- Existing LOB designs (academic papers / open-source engines)
- Course materials on parallel synchronization

---

## Goals and Deliverables

### Plan to Achieve
- Implement a correct single-threaded LOB simulator
- Implement coarse-grained locking version
- Implement fine-grained locking version
- Implement batching-based approach
- Measure throughput and scalability across thread counts

### Hope to Achieve
- Explore lock-free or hybrid approaches
- Analyze contention patterns under skewed workloads
- Achieve measurable speedup over baseline

### Evaluation Questions
- What limits scalability in each design?
- Does reducing synchronization frequency outperform finer locking?
- How does workload skew affect performance?

---

## Platform Choice

We choose multi-core CPU parallelism because:

- The workload involves shared mutable state, making it a good fit for studying synchronization
- CPUs provide flexible threading and memory models
- The problem is not well-suited for GPUs due to irregular control flow and dependencies

---

## Schedule

- **Week 1**: Baseline LOB + correctness testing
- **Week 2**: Coarse + fine-grained locking
- **Week 3**: Batching + optimization
- **Week 4**: Experiments + profiling
- **Week 5**: Final analysis + report

**Milestone (April 14):**
- Working baseline + at least one parallel version
- Initial performance results

---

🔙 [Back to Main Page](index.html)