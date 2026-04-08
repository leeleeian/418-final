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
#### 1.  Controlled Baseline Generator: 
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

 #### 2. Stateful Market Generator: 
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

#### 3. Generator Event-Based: 
   Generate events relative to the current book state i.e. 
   with some probability:
   - place a passive order at level `best_bid - k`
   - place an aggressive buy at `best_ask` or above
   - cancel an order currently resting within the top `d` levels
   - modify an existing order by cancel-and-reinsert
   - send large market order, sweeping across multiple levels

  This will really test contention in our LOB to concentrate pressure.

-----
## Baseline Infra/Approach:
Per-ticker arrival-order matching: within a symbol, messages must be processed in the order
the generator emitted them, and the resulting trades + final book state must be identical 
(in the future) to the sequential implementation. 

Thus, `trades.json` is keyed with 'byTicker', then within each array, order is execution 
order for that symbol. A parallel implementation that processes symbols on different 
threads will still produce the same per-symbol arrays as long as it preserves arrival order 
within each book. `books.json` lists every resting order per symbol in price ascending,
FIFO order. Then, if parallel matcher ever drops, duplicates, or reorders an order within a level,
the diff catches it immediately.

----

## Current Impl / Understanding current codebase structure:
- code/main.cpp -- driver: Builds a `GeneratorConfig`, runs `OrderGenerator::generateAll()`,
  hands the messages to `MatchingEngine::processAll()`, then prints throughput, trade count,
  per-ticker book state and few sample trades.
- Main idea: queue is the synchronization boundary; everything below it remains single-threaded by construction.


### How to Test:
From the repo root we can use the following commands to test:
- `make` (or `make build`): compiles everything into `build/sim`, incremental -- only rebuilds what changed
- `make run`: build (if needed) with default config (no dumps)
- `make dump`: build + run, writes to `build/dump/{orders, trades, books}.json`
- `make baseline`: build + run, write `golden/{trades,books}.json` + `run.log` (run once + commit)
- `make verify`: build + run + diff against `golden/`, exit nonzero on any mismatch
- `make clean`: delete `build/` directory, doesn't touch `golden/`

CLI glafs on binary itself (used by targets): \
./build/sim --help \
&nbsp; --seed N            RNG seed (default 42) \
&nbsp; --num-orders N      messages in main stream (default 50000) \
&nbsp; --dump-orders PATH  write generated order stream as JSON \
&nbsp; --dump-trades PATH  write executed trades, grouped by ticker, as JSON \
&nbsp; --dump-books  PATH  write final book state per ticker as JSON

The `BASELINE_SEED` and `BASELINE_NUM_ORDERS` can be toggled in Makefile for harness's input -- so golden file
is reproducible from single command


Note: for makefile, supports:
- auto-discovery supported ie picks up any new `.cpp` added
- header dependency tracking
- mirrored object layout i.e. .cpp -> .o
- override friendly

*build files are added to .gitignore so artifact files not added

----------------------------------------

## Notes going into Week 2 + later:
- Will need a command-queue + worker layer when we go multi-threaded: producers submit 
`OrderMessage`'s into a bounded thread-safe queue and a single worker with drain them into the matching engine. Then that layer will need to serialize concurrent producers
- For future parallel versions, workflow can stay similarly: build parallel binary + run make-verify
  
#### Concerns: 
- Currently pins one workload, the (seed=42, num-orders=50000, 3 tickers) config. Once we start writing parallel implementations, probably want a few golden files at different scales / ticker counts to stress different code paths. The Makefile: BASELINE_SEED=7 BASELINE_NUM_ORDERS=1000000 make baseline writes a different golden, and you'd just check in a few of them under e.g. golden/seed-42-50k/.
- It can't catch races that happen to produce a valid output (a parallel matcher might sometimes get lucky). For that you'd want repeated runs or a stress harness.

----

## Per-file Reference

Files are listed in dependency order: each layer builds only on the layers above it.
`Types → Order → LimitOrderBook → OrderGenerator → MatchingEngine → main`.

### `code/LimitOrderBook/Types.h`

Shared primitive types used by every other module. Single source of truth so the
generator, book, and engine can't drift on widths.

```cpp
enum class Side { BUY, SELL };

using Id       = std::uint64_t;   // unique order id
using Price    = std::uint32_t;   // integer ticks (e.g. cents)
using Quantity = std::uint32_t;   // shares / contracts
```

---

### `code/LimitOrderBook/Order.{h,cpp}`

The state of a single resting order. Owned via `OrderPointer`
(`shared_ptr<Order>`) so the book can hold the same order in two places — the
global id index and the price-level FIFO — without copies.

```cpp
enum class OrderStatus { WAITING_ACK, PARTIAL_FILL, COMPLETE_FILL };

class Order {
  Id id;
  Side side;
  Price price;
  Quantity initQ;     // initial quantity
  Quantity leftQ;     // remaining (post-fills) quantity
  OrderStatus status;

public:
  Order(Id id, Side side, Price price, Quantity initQ);

  // accessors
  Id          getId() const;
  Side        getSide() const;
  Price       getPrice() const;
  Quantity    getInitialQuantity() const;
  Quantity    getRemainingQuantity() const;
  OrderStatus getOrderStatus() const;

  // Apply a fill of size `q`. Throws if q > remaining. Updates status to
  // PARTIAL_FILL or COMPLETE_FILL automatically.
  void fill(const Quantity q);
};

using OrderPointer = std::shared_ptr<Order>;
```

---

### `code/LimitOrderBook/LimitOrderBook.{h,cpp}`

Single-symbol matching book with price-time priority. Holds all resting orders
and is the only place orders are mutated.

**Internal layout:**
- `bids_` / `asks_` : `std::map<Price, PriceLevelPointer>` (red-black tree).
  Best bid = `prev(end())`, best ask = `begin()`. O(log M) level insert/erase,
  O(1) best access.
- `PriceLevel` : `std::list<OrderPointer>` for FIFO time priority +
  `unordered_map<Id, list::iterator>` for O(1) cancel inside the level.
- `orders_` : `unordered_map<Id, OrderPointer>` for O(1) global id lookup.

**Trade and snapshot types** (defined in this header so any user of the book
can name them):

```cpp
struct Trade {
  Id buyOrderId;
  Id sellOrderId;
  Price price;
  Quantity quantity;
  std::string ticker;     // stamped by MatchingEngine, blank inside the book
};

struct BookSnapshot {                  // O(N) deep copy for inspection / verification
  struct OrderView { Id id; Quantity remaining; };
  struct LevelView { Price price; std::vector<OrderView> orders; };
  std::vector<LevelView> bids;         // ascending price, FIFO inside each level
  std::vector<LevelView> asks;
};
```

**Public API:**

```cpp
class LimitOrderBook {
public:
  // Match incoming order against crossing liquidity, then rest any remainder.
  // Returns trades in execution order.
  std::vector<Trade> addLimitOrder(Id id, Side side, Price price, Quantity quantity);

  // Walk the opposite side at any price until filled or empty. Unfilled
  // remainder is dropped (market orders never rest).
  std::vector<Trade> addMarketOrder(Id id, Side side, Quantity quantity);

  // O(1) cancel via the level's id->iterator map. No-op if id is unknown.
  void cancelOrder(Id id);

  // Cancel + re-add. Modified order goes to the back of its (possibly new)
  // price level — loses time priority, standard exchange behaviour.
  std::vector<Trade> modifyOrder(Id id, Price newPrice, Quantity newQuantity);

  // Inspectors
  bool         hasOrder(Id id) const;
  std::size_t  restingOrderCount() const;
  std::size_t  bidLevelCount() const;
  std::size_t  askLevelCount() const;
  BookSnapshot snapshot() const;       // deterministic full read-out
};
```

**Internal helpers (in the `.cpp`):**
- `matchBuyAgainstAsks(incoming, maxPrice, trades)` — walks asks ascending,
  fills until incoming is exhausted or no ask `<= maxPrice` remains.
- `matchSellAgainstBids(incoming, minPrice, trades)` — symmetric, walks bids
  descending.
- `rest(order)` — appends to FIFO at the order's price level (creating the
  level if absent) and registers the id globally.

The book is **single-threaded**. Concurrency is the job of a queue/worker
layer that will sit on top, not the book.

---

### `code/OrderGenerator/OrderGenerator.{h,cpp}`

Deterministic synthetic order stream for benchmarks and correctness checks.
Produces messages for one or more tickers; same seed → identical stream.

**Message format** (the protocol the engine consumes):

```cpp
enum class OrderType  { LIMIT, MARKET };
enum class ActionType { NEW, CANCEL };

struct OrderMessage {
  ActionType  action;
  OrderType   orderType;
  Id          orderId;
  std::string ticker;
  Side        side;
  Price       price;        // 0 for market orders / cancels
  Quantity    quantity;     // 0 for cancels
};
```

**Generator config** (all knobs in one struct):

```cpp
struct GeneratorConfig {
  uint64_t seed = 42;
  std::vector<std::string> tickers = {"AAPL"};
  Price defaultMidPrice = 10000;
  std::unordered_map<std::string, Price> midPrices;   // per-ticker overrides
  Price tickSize = 1;
  size_t numOrders = 10000;                           // size of main stream
  double limitRatio = 0.60, marketRatio = 0.20, cancelRatio = 0.20;
  int maxPriceOffsetTicks = 20;                       // limit-price spread around mid
  Quantity minQuantity = 1, maxQuantity = 100;
  size_t initialDepthPerSide = 10;                    // seeded resting orders per side
};
```

**Generator class** — two phases of output:

```cpp
class OrderGenerator {
public:
  explicit OrderGenerator(const GeneratorConfig& config);

  // Phase 1: symmetric depth around each ticker's mid (limit orders only).
  std::vector<OrderMessage> generateInitialBook();

  // Phase 2: numOrders messages drawn from the limit/market/cancel mix.
  std::vector<OrderMessage> generateOrders();

  // Convenience: phase 1 + phase 2 concatenated.
  std::vector<OrderMessage> generateAll();

  // Offline serialization (CSV; main.cpp adds JSON variants).
  static void writeToCSV(const std::string& filepath,
                         const std::vector<OrderMessage>& messages);
};
```

Cancels target ids the generator believes are still resting (best-effort — if
the engine has already filled them, the cancel is a no-op). Order ids are
globally unique across tickers (one shared counter).

---

### `code/MatchingEngine/MatchingEngine.{h,cpp}`

Stateless front-end that translates `OrderMessage`s into book operations. Owns
one `LimitOrderBook` per ticker, lazily created on first message.
Single-threaded for v1; the threaded queue/worker layer described in the spec
is intended to wrap this class, not live inside it.

```cpp
class MatchingEngine {
public:
  // Apply one message; returns trades produced by it (empty for cancels).
  std::vector<Trade> onMessage(const OrderMessage& msg);

  // Apply a batch in arrival order, concatenating trades from all messages.
  std::vector<Trade> processAll(const std::vector<OrderMessage>& msgs);

  // Read-only access to a per-ticker book; nullptr if the ticker has never
  // appeared in any message.
  const LimitOrderBook* bookFor(const std::string& ticker) const;

private:
  LimitOrderBook& bookForMut(const std::string& ticker);   // lazy create
  std::unordered_map<std::string, std::unique_ptr<LimitOrderBook>> books_;
};
```

**Dispatch logic** (in `onMessage`):
- `NEW` + `LIMIT`  → `book.addLimitOrder(id, side, price, qty)`
- `NEW` + `MARKET` → `book.addMarketOrder(id, side, qty)`
- `CANCEL`         → `book.cancelOrder(id)` and return `{}`

After the book call, every produced `Trade` has its `ticker` field stamped
with `msg.ticker` — the book itself is symbol-agnostic, so the engine is the
only place that knows which symbol a trade belongs to.

---

### `code/main.cpp`

End-to-end driver and CLI tool. Wires generator → engine → books, prints
throughput / sample trades, and writes JSON dumps for the golden-trace
correctness harness.

**CLI flags:**

| Flag                 | Default | Meaning                                       |
| -------------------- | ------- | --------------------------------------------- |
| `--seed N`           | `42`    | RNG seed for the generator                    |
| `--num-orders N`     | `50000` | size of the main order stream                 |
| `--dump-orders PATH` | –       | write generated input as JSON                 |
| `--dump-trades PATH` | –       | write trades grouped by ticker as JSON        |
| `--dump-books PATH`  | –       | write final book state per ticker as JSON    |
| `-h`, `--help`       | –       | usage                                         |

**Key local helpers** (file-scope `namespace {}`):

```cpp
const char* toStr(ActionType);                                    // "NEW" / "CANCEL"
const char* toStr(OrderType);                                     // "LIMIT" / "MARKET"
const char* toStr(Side);                                          // "BUY" / "SELL"

void dumpOrdersJson(const std::string& path,
                    const std::vector<OrderMessage>& msgs);       // raw input stream

void dumpTradesJson(const std::string& path,
                    const std::vector<Trade>& trades);            // grouped by ticker

void dumpBooksJson(const std::string& path,
                   const MatchingEngine& eng,
                   const std::vector<std::string>& tickers);      // BookSnapshot per book

CliOptions parseArgs(int argc, char** argv);                      // tiny argv parser
```

**`main` flow:**

1. Parse CLI flags into a `CliOptions`.
2. Build a `GeneratorConfig` (3 tickers — AAPL/MSFT/GOOG — with distinct mids).
3. `OrderGenerator::generateAll()` → vector of `OrderMessage`.
4. Optionally `dumpOrdersJson(...)`.
5. `MatchingEngine::processAll(messages)` → vector of `Trade`, timed with
   `std::chrono::steady_clock`.
6. Print throughput (`msgs/sec`), trade count, per-ticker resting/level
   counts, and the first 5 trades.
7. Optionally `dumpTradesJson(...)` and `dumpBooksJson(...)`.

The JSON dumps are written with deterministic ordering (sorted ticker keys,
FIFO inside each level) so a `diff` against the checked-in `golden/`
directory works as a complete regression check via `make verify`.