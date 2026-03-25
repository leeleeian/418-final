# Project Proposal: Parallel Limit Order Book Simulation

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## URL

landing page: https://leeleeian.github.io/418-final/

---
## Summary

We are going to build a parallel simulation of a limit order book (LOB) to study how different synchronization strategies impact scalability under realistic financial workloads. Our implementation will explore multi-core CPU parallelism using threading and synchronization primitives.

---

## Background

A limit order book is the core data structure used by financial exchanges to match buy and sell orders. Orders are processed according to price-time priority, meaning that better-priced orders are matched first, and among equal prices, earlier orders take precedence.

To maintain this priority, the system typically maintains two ordered structures: a Bid book (buyers, sorted descending by price) and an Ask book (sellers, sorted ascending by price). Each of these structures is often implemented as a sparse list of "price levels" where each price level contains a FIFO (to account for time precedence) linked list of individual orders.

The LOB will also have three main operations: inserting new orders, cancelling existing orders, and an internal operation to fill orders if there are any bids and asks with matching prices.  

Order books are used for exchanges that process millions of orders per second. The high volume of data, as well as its time sensitivity, make parallelizing LOBs to optimize performance, a very useful task. Moreover, there are many parts of this system that have opportunities for parallelism. For example, operations on different tickers will not affect one another, since they are on different order books, making them very data-parallel. Additionally, parsing the messages associated with each order (placement or cancellation) could be pipelined to hide latency. 

However within each ticker, the matching engine introduces strong dependencies that limit parallelism. For example, a single, but large, market order can lead to many matches across multiple price levels, leading to a long read-modify-write chain that alters the global state. Similarly, since deterministic ordering of messages is legally required by exchanges, it is pertinent that order inserts and cancellations lead to safe and synchronized access of shared memory structures. This makes the LOB a compelling case study for understanding the tradeoffs between concurrency and correctness.

---

## The Challenge

The primary challenge lies in maintaining correctness while introducing parallelism. 

**Dependencies**: The main dependency we must navigate is that matching bids and asks depends on a strict price-time priority of orders that must be maintained across a globally shared state. As such, it is important to maintain a deterministic ordering, and good synchronization of all the processors.

**Constraints and System Mapping**: Mapping this workload is particularly challenge because most trading activity occurs near the best bid/ask, leading to potential for contention and/or uneven load balancing. 

**Memory Access Characteristics**: The workload has irregular memory access patterns and bad spatial locality because the order book contains pointers for orders within a price level, and threads must access this shared mutable structure to perform all of the LOB operations.

**Communication to Computation Ratio**: The communication-to-computation ratio may be high since the actual computation mainly only involves comparing integer prices and subtracting order quantities, but much communication is required to safely update the shared mutable state with the results of this computation. 

**Contention and Synchronization Overhead**: Since there are many small operations and a large number of concurrent agents in certain memory location (associated with the best bid/ask), having a single lock can lead to a lot of contention. Fine-grained locking could be a good way to address this, however that would lead to additional synchronization overhead from lock acquisation and cache coherence traffic, particularly if there is false sharing with adjacent price levels sitting on the same cache line. As such, there is opportunity to analyze the tradeoff between concurrency and synchronization overhead.


Overall, naive approaches such as coarse-grained locking severely limit scalability while fine-grained locking and lock-free data structures introduce overhead from synchronization, retries, and cache coherence effects. Additionally, workload skew exacerbates contention at hot spots.As such, we aim to explore whether restructuring computation, i.e. through batching, agent-level parallelism, and/or partitioning, can reduce synchronization frequency and improve throughput.

---

## Resources

We will implement the system in C++ using multithreading (`std::thread` or pthreads).

We will use:
- CMU lab machines (multi-core CPUs)
- Possibly PSC clusters if needed

We will build from scratch but reference:
- Course materials on parallel systems
- Existing Implementations: https://github.com/brprojects/Limit-Order-Book and https://github.com/devmenon23/Limit-Order-Book 
- Academic papers on LOBs: A Deterministic Limit Order Book Simulator with Hawkes-Driven Order Flow (https://arxiv.org/abs/2510.08085)

---

## Goals and Deliverables

### Plan to Achieve
- Correct single-threaded C++ LOB simulator that maintains price-time priority
- Parallel implementation using coarse-grained locking that partitions orders for different tickers across different processors
- Parallel implementation using fine-grained locking that allows concurrent modification to the same order book (at individual price-levels)
- Analyze and evaluate throughput and scalability
- Performance Goal 1: We aim to achieve over a 4x speedup on an 8-core CPU for the coarse-grained approach, because different tickers can be processed entirely independently without much synchronization overhead, making >4x speedup a reasonable target
- Performance Goal 2: We aim to achieve over a 3x speedup with the fine-grained locking implementation over the baseline, because there will be high contention around the best bid/ask, leading to inherent serialization in addition to lock acquisition overhead, making sub-linear speedup likely

### Hope to Achieve
- Explore hybrid or lock-free approaches
- Implement a batching-based approach that groups independent orders
- Analyze contention under skewed workloads

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

We choose C++ (particularly `std::thread` or OpenMP) as our language of choice, because LOBs rely heavily on shared mutable state, branching, and low-latency pointer traversal. C++ provides fine-grained control over memory models, atomics, and synchronization primitives that allow for usch optimizations on lock-free or fine-grained locking data structures. 

We choose to use multi-core CPUs (the GHC lab machines) as our computer of choice because order matching is very data-dependent, and may lead to lots of divergent execution. Particularly, one thread processing an order with an existing price match in the LOB may execute immediately while another thread may have to insert its order into the book, which could lead to a pointer traversal. These drastically different possible paths of execution would perform poorly on a GPU, since they prevent good SIMD utilization due to the irregular control flow they bring.

---

## Schedule

- **Week 1 (Ddl: March 31)**: Implement a correct, baseline LOB. Set up data generation scripts to create order flows for correctness tests.
- **Week 2 (Ddl: April 7)**: Implement a parallel LOB with coarse grained locking. Set up scripts for benchmarking that measure throughput and latency as well as speedup/scalability.
- **Week 3 (Ddl: April 14)**: Begin implementing a parallel LOB with fine-grained locking. Draft and submit milestone report, including preliminary speedup results for coarse grained locking.
- **Week 4 (Ddl: April 21)**: 
    - Finish fine grained implementation, including optimizations such as padding data structures to avoid false sharing 
    - Evaluate fine grained implementation under high workload skew
    - Hope To Achieve: Implement the batching based approach to group independent orders and reduce synchronization
- **Week 5 (Ddl: April 28)**: 
    - Run final evaluations across varying levels of contention
    - Generate execution time and speedup graphs for results
    - Hope To Achieve: Finish and profile batching based approach
- **Week 6 (Ddl: April 30/May 1)**: 
    - Write Final report + Poster Session
    - Hope To Achieve: Include results from batching based approach in report

**Milestone (April 14):**
- Baseline LOB + coarse grained parallel version of LOB
- Initial results

---

🔙 [Back to Home](index.html)