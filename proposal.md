# Parallel Limit Order Book Simulation

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**CS 418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## URL

landing page: https://leeleeian.github.io/418-final/

---

## Summary

We propose to build a parallel simulation of a limit order book (LOB) to study how different synchronization strategies impact scalability under realistic financial workloads. Our implementation will explore multi-core CPU parallelism using threading and synchronization primitives.

---

## Background

A limit order book is the core data structure used by financial exchanges to match buy and sell orders. Orders are processed according to price-time priority, meaning that better-priced orders are matched first, and among equal prices, earlier orders take precedence.

The system maintains two ordered structures (bids and asks), and operations include order insertion, cancellation, and matching. These operations are highly interdependent, as each update modifies global state and affects subsequent matching behavior.

While parts of the system, such as order generation, are naturally parallelizable, the matching engine introduces strong dependencies that limit parallelism. This makes the LOB a compelling case study for understanding the tradeoffs between concurrency and correctness.

---

## The Challenge

The primary challenge lies in maintaining correctness while introducing parallelism.

- **Dependencies**: Matching depends on strict price-time priority
- **Contention**: Most activity occurs near the best bid/ask
- **Memory behavior**: Irregular access patterns and shared mutable state
- **Synchronization overhead**: Locking and coordination dominate small operations

Naive approaches such as coarse-grained locking severely limit scalability. Fine-grained locking and lock-free data structures introduce overhead from synchronization, retries, and cache coherence effects. Additionally, workload skew exacerbates contention at hot spots. 

Fine-grained locking is motivated by the need to expose parallelism that is hidden by coarse-grained synchronization. In a limit order book, many operations such as inserting limit orders at different price levels appear independent and could, in principle, be executed concurrently. By associating locks with smaller regions of the data structure (e.g., individual price levels), fine-grained locking attempts to reduce unnecessary serialization and allow multiple threads to operate in parallel. This approach is particularly appealing in workloads with many small operations and a large number of concurrent agents, where a single global lock would become a scalability bottleneck.

However, this increased concurrency comes at the cost of additional synchronization overhead, more complex coordination, and potential contention at frequently accessed regions of the book (such as the best bid and ask). As a result, fine-grained locking represents a tradeoff between reducing contention and increasing coordination cost, and its effectiveness depends heavily on workload characteristics such as skew and access patterns.

We aim to explore whether restructuring computation, i.e. through batching, agent-level parallelism, and/or partitioning, can reduce synchronization frequency and improve throughput.

---

## Resources

We will implement the system in C++ using multithreading (`std::thread` or pthreads).

We will use:
- CMU lab machines (multi-core CPUs)
- Possibly PSC clusters if needed

We will build from scratch but reference:
- Academic papers on limit order books
- Course materials on parallel systems

Maybe (?) codebases (do we need request to start from their codebase):
- https://github.com/brprojects/Limit-Order-Book 
- https://github.com/devmenon23/Limit-Order-Book 
- C++ specific paper: A Deterministic Limit Order Book Simulator with Hawkes-Driven Order Flow (https://arxiv.org/abs/2510.08085)

---

## Goals and Deliverables

### Plan to Achieve
- Correct single-threaded LOB simulator
- Coarse-grained locking implementation
- Fine-grained locking implementation
- Batching-based approach
- Throughput and scalability evaluation

### Hope to Achieve
- Explore hybrid or lock-free approaches
- Analyze contention under skewed workloads
- Achieve meaningful speedup over baseline

### High Reach Goals (with very low probability)
- Use real data in low-latency environment (from simulation to forecasting): https://github.com/OpenHFT/Chronicle-Queue -> might be interesting to eventually benchmark on a small-scale to real life environment.
- Industry practice: Parallel Neural Hawkes Processes - [State Dependent Parallel Lock-Free Transactional Transformation (LFTT)](state-dependent-parallel-LOB.pdf)
- JAX-LOB: A purely functional, hardware-accelerated simulator for Reinforcement Learning (RL) environments

### Evaluation Questions
- What limits scalability in each approach?
- Does batching outperform fine-grained locking?
- How does workload skew impact performance?

---

## Platform Choice

We choose multi-core CPU parallelism because:

- The workload involves shared mutable state
- Fine control over synchronization primitives
- The problem is not well-suited for GPUs due to dependencies and irregular control flow

---

## Schedule

- **Week 1**: Baseline LOB + correctness
- **Week 2**: Coarse + fine-grained locking
- **Week 3**: Batching + optimization
- **Week 4**: Experiments + profiling
- **Week 5**: Final report

**Milestone (April 14):**
- Baseline + one parallel version
- Initial results

---

🔙 [Back to Home](index.html)