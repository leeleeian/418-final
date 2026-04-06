# Week 1: Parallel Limit Order Book Simulation 

**Irene Liu (irenel), Lillian Yu (lyu2)**  
**15-418 – Spring 2026**

🔙 [Back to Home](index.html)

---

## Goals 

- Implement a correct, baseline LOB
- Set up data generation scripts to create order flows for correctness tests

Deadline: March 31st

---

## Order Generation as Experimental Design

To create test inputs, since we want to test how syncrhonization strategies behave
under realistic contention, our generator should support functionalities that can
vary the intensity of such contention variables such as: concentration near the
bid-ask spread, burstiness, cancel density, sweeping market orders, and multi-ticker skew.
This lines up with the bottlenecks that we want to target in our project: 
hot spots near bid ask spread, strict price-time dependency enforcement, skewed workloads, 
and irregular shared-memory updates.

Our two resources may inspire some implementation details:
1. (https://github.com/devmenon23/Limit-Order-Book) Markov-chain market state model plus
   Pareto-dstributed sizes/prices, explicity creating shifts of buy/sell pressure and
   heavy-tailed, bursty order flow.
2. (https://github.com/brprojects/Limit-Order-Book) Initial seeded book, requests with prices
   centered around a moving mid, average active book depth targets, and mixed order types.

Plan for implementation: Generator Ladder
1. #### Controlled Baseline Generator: 
   This aims to just verify correctness and 
   isolate performance effects. Sampling from identically independent distribution
   involving
   - fixed tick size
   - one or several tickers
   - seeded initial book with symmetric depth around a reference midprice
   - configurable mix of limit, market, cancel, and modify
   - prices sampled as offsets from current mid price
   - sizes from either a light-tailed or bounded distribution
  
  This gives a first simple baseline for correctness and coarse-grained locking benchmarks.

2. #### Stateful Market Generator: 
   Markovian process alternating the market to have
   - neutral
   - buy pressure
   - sell pressure
   - high-volatility burst
   - cancellation storm
   - liquidity drought
  Then, for example, for buy-pressure state we might:
  - increase buy arrival rate
  - move buy limit prices closer to or through the ask
  - increase market buy probability
  - reduce passive sell replenishment

  This will be a good hard scenario to test our fine-grained design on.

3. #### Generator Event-Based: 
   Generate events relative to the current book state i.e. 
   with some probability:
   - place a passive order at level `best_bid - k`
   - place an aggressive buy at `best_ask` or above
   - cancel an order currently resting within the top `d` levels
   - modify an existing order by cancel-and-reinsert
   - send large market order, sweeping across multiple levels

  This will really test contention in our LOB to concentrate pressure.
