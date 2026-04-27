// End-to-end driver: generator -> matching engine -> books.
//
// Build: see top-level Makefile.
//
// Usage:
//   ./build/sim                                  # run with defaults
//   ./build/sim --seed 42                        # change RNG seed
//   ./build/sim --num-orders 100000              # change order count
//   ./build/sim --dump-orders out/orders.json    # write generated input stream
//   ./build/sim --dump-trades out/trades.json    # write trades grouped by ticker
//   ./build/sim --dump-books  out/books.json     # write final book state per ticker
//   ./build/sim --engine coarse                  # mutex-wrapped books (ST feed)
//   ./build/sim --engine coarse --parallel       # per-ticker shards + thread pool
//   ./build/sim --engine fine                    # per-level fine-grained locks (ST feed)
//   ./build/sim --engine fine --parallel         # per-level locks + thread pool
//
// All dump files are deterministic for a given seed/config. They are also
// stable across implementations that preserve per-ticker arrival order, which
// is exactly the correctness invariant any matching engine (sequential or
// parallel) must satisfy. That makes a plain `diff` against a checked-in
// golden file a complete regression check — see `make baseline` / `make verify`.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "LimitOrderBook/LimitOrderBook.h"
#include "MatchingEngine/CoarseGrainedMatchingEngine.h"
#include "MatchingEngine/FineGrainedMatchingEngine.h"
#include "MatchingEngine/MatchingEngine.h"
#include "OrderGenerator/OrderGenerator.h"

namespace {

/* ------------------------------------------------------------------ */
/* enum -> string                                                      */
/* ------------------------------------------------------------------ */

const char* toStr(ActionType a) {
  switch (a) {
    case ActionType::NEW:    return "NEW";
    case ActionType::CANCEL: return "CANCEL";
  }
  return "UNKNOWN";
}
const char* toStr(OrderType t) {
  switch (t) {
    case OrderType::LIMIT:  return "LIMIT";
    case OrderType::MARKET: return "MARKET";
  }
  return "UNKNOWN";
}
const char* toStr(Side s) {
  switch (s) {
    case Side::BUY:  return "BUY";
    case Side::SELL: return "SELL";
  }
  return "UNKNOWN";
}

/* ------------------------------------------------------------------ */
/* JSON dumpers                                                        */
/* ------------------------------------------------------------------ */
//
// Hand-rolled rather than pulling in a JSON library. The data is closed:
// no embedded quotes, backslashes, or non-ASCII to escape. Layout is fixed
// (one record per line, sorted keys) so the files diff cleanly.

void dumpOrdersJson(const std::string& path, const std::vector<OrderMessage>& msgs) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("dumpOrdersJson: cannot open " + path);

  out << "{\n";
  out << "  \"count\": " << msgs.size() << ",\n";
  out << "  \"orders\": [";
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    const auto& m = msgs[i];
    out << (i == 0 ? "\n    " : ",\n    ")
        << "{\"action\":\""   << toStr(m.action)
        << "\",\"type\":\""   << toStr(m.orderType)
        << "\",\"id\":"       << m.orderId
        << ",\"ticker\":\""   << m.ticker
        << "\",\"side\":\""   << toStr(m.side)
        << "\",\"price\":"    << m.price
        << ",\"quantity\":"   << m.quantity
        << "}";
  }
  out << "\n  ]\n}\n";
}

void dumpTradesJson(const std::string& path, const std::vector<Trade>& trades) {
  // Bucket by ticker. std::map gives alphabetical key order in the output.
  std::map<std::string, std::vector<const Trade*>> byTicker;
  for (const auto& t : trades) byTicker[t.ticker].push_back(&t);

  std::ofstream out(path);
  if (!out) throw std::runtime_error("dumpTradesJson: cannot open " + path);

  out << "{\n";
  out << "  \"totalTrades\": " << trades.size() << ",\n";
  out << "  \"byTicker\": {";
  bool firstTicker = true;
  for (const auto& kv : byTicker) {
    out << (firstTicker ? "\n    \"" : ",\n    \"") << kv.first << "\": [";
    firstTicker = false;
    for (std::size_t i = 0; i < kv.second.size(); ++i) {
      const Trade& t = *kv.second[i];
      out << (i == 0 ? "\n      " : ",\n      ")
          << "{\"buy\":"    << t.buyOrderId
          << ",\"sell\":"   << t.sellOrderId
          << ",\"price\":"  << t.price
          << ",\"qty\":"    << t.quantity
          << "}";
    }
    out << "\n    ]";
  }
  out << "\n  }\n}\n";
}

void writeLevelArray(std::ostream& out,
                     const std::vector<BookSnapshot::LevelView>& levels) {
  for (std::size_t i = 0; i < levels.size(); ++i) {
    const auto& level = levels[i];
    out << (i == 0 ? "\n        " : ",\n        ")
        << "{\"price\":" << level.price << ",\"orders\":[";
    for (std::size_t j = 0; j < level.orders.size(); ++j) {
      const auto& o = level.orders[j];
      if (j > 0) out << ",";
      out << "{\"id\":" << o.id << ",\"remaining\":" << o.remaining << "}";
    }
    out << "]}";
  }
}

template <typename BookEngine>
void dumpBooksJson(const std::string& path, const BookEngine& eng,
                   const std::vector<std::string>& tickers) {
  // Sort tickers so output is deterministic regardless of cfg ordering.
  std::vector<std::string> sorted(tickers);
  std::sort(sorted.begin(), sorted.end());

  std::ofstream out(path);
  if (!out) throw std::runtime_error("dumpBooksJson: cannot open " + path);

  out << "{";
  bool first = true;
  for (const auto& ticker : sorted) {
    const auto* book = eng.bookFor(ticker);
    if (!book) continue;
    auto snap = book->snapshot();

    out << (first ? "\n  \"" : ",\n  \"") << ticker << "\": {";
    first = false;
    out << "\n    \"restingOrders\": " << book->restingOrderCount() << ",";
    out << "\n    \"bids\": [";
    writeLevelArray(out, snap.bids);
    out << "\n    ],";
    out << "\n    \"asks\": [";
    writeLevelArray(out, snap.asks);
    out << "\n    ]";
    out << "\n  }";
  }
  out << "\n}\n";
}

/* ------------------------------------------------------------------ */
/* CLI parsing                                                         */
/* ------------------------------------------------------------------ */

enum class EngineKind { SEQUENTIAL, COARSE, FINE };

struct CliOptions {
  std::uint64_t seed = 42;
  std::size_t numOrders = 50000;
  std::size_t numTickers = 3;
  std::string dumpOrders;
  std::string dumpTrades;
  std::string dumpBooks;
  EngineKind engine = EngineKind::SEQUENTIAL;
  bool parallel = false;
  std::size_t threads = 0;
  std::string workload = "balanced";  // balanced | crossing | resting
};

[[noreturn]] void usageAndExit(const char* prog, int code) {
  std::cerr <<
    "usage: " << prog << " [options]\n"
    "  --seed N            RNG seed (default 42)\n"
    "  --num-orders N      messages in main stream (default 50000)\n"
    "  --num-tickers N     ticker/shard count (default 3)\n"
    "  --engine NAME       sequential | coarse | fine (default sequential)\n"
    "  --parallel          use per-ticker parallel feed (coarse/fine engines only)\n"
    "  --threads N         worker threads for --parallel (0 = hardware default)\n"
    "  --workload TYPE     balanced | crossing | resting (default balanced)\n"
    "  --dump-orders PATH  write generated order stream as JSON\n"
    "  --dump-trades PATH  write executed trades, grouped by ticker, as JSON\n"
    "  --dump-books  PATH  write final book state per ticker as JSON\n"
    "  -h, --help          show this message\n";
  std::exit(code);
}

CliOptions parseArgs(int argc, char** argv) {
  CliOptions opts;
  auto need = [&](int i) -> std::string {
    if (i + 1 >= argc) {
      std::cerr << "error: " << argv[i] << " requires an argument\n";
      usageAndExit(argv[0], 2);
    }
    return std::string(argv[i + 1]);
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if      (arg == "--seed")         { opts.seed       = std::stoull(need(i)); ++i; }
    else if (arg == "--num-orders")   { opts.numOrders  = std::stoull(need(i)); ++i; }
    else if (arg == "--num-tickers")  { opts.numTickers = std::stoull(need(i)); ++i; }
    else if (arg == "--engine") {
      std::string name = need(i);
      ++i;
      if (name == "sequential") opts.engine = EngineKind::SEQUENTIAL;
      else if (name == "coarse") opts.engine = EngineKind::COARSE;
      else if (name == "fine") opts.engine = EngineKind::FINE;
      else {
        std::cerr << "error: --engine must be sequential, coarse, or fine\n";
        usageAndExit(argv[0], 2);
      }
    }
    else if (arg == "--parallel")     { opts.parallel = true; }
    else if (arg == "--threads")      { opts.threads = std::stoull(need(i)); ++i; }
    else if (arg == "--workload") {
      std::string name = need(i);
      ++i;
      if (name == "balanced" || name == "crossing" || name == "resting") {
        opts.workload = name;
      } else {
        std::cerr << "error: --workload must be balanced, crossing, or resting\n";
        usageAndExit(argv[0], 2);
      }
    }
    else if (arg == "--dump-orders")  { opts.dumpOrders = need(i);              ++i; }
    else if (arg == "--dump-trades")  { opts.dumpTrades = need(i);              ++i; }
    else if (arg == "--dump-books")   { opts.dumpBooks  = need(i);              ++i; }
    else if (arg == "-h" || arg == "--help") { usageAndExit(argv[0], 0); }
    else {
      std::cerr << "error: unknown flag '" << arg << "'\n";
      usageAndExit(argv[0], 2);
    }
  }
  return opts;
}

template <typename BookEngine>
void printBookSummary(const BookEngine& eng, const std::string& ticker) {
  const auto* book = eng.bookFor(ticker);
  if (!book) { std::cout << "  " << ticker << ": <no book>\n"; return; }
  std::cout << "  " << ticker
            << ": resting=" << book->restingOrderCount()
            << "  bidLevels=" << book->bidLevelCount()
            << "  askLevels=" << book->askLevelCount() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions opts = parseArgs(argc, argv);
  if (opts.numTickers == 0) {
    std::cerr << "error: --num-tickers must be >= 1\n";
    return 2;
  }
  if (opts.parallel && opts.engine == EngineKind::SEQUENTIAL) {
    std::cerr << "error: --parallel requires --engine coarse or fine\n";
    return 2;
  }

  // ---------- 1. Configure the generator ----------
  GeneratorConfig cfg;
  cfg.seed = opts.seed;
  const std::vector<std::pair<std::string, Price>> presets = {
      {"AAPL", 17500}, {"MSFT", 42000}, {"GOOG", 13800}, {"AMZN", 18200},
      {"NVDA", 90000}, {"META", 51000}, {"TSLA", 19000}, {"NFLX", 65000},
      {"INTC", 3600},  {"AMD", 17000},  {"ORCL", 12500}, {"IBM", 19000},
      {"CRM", 30500},  {"ADBE", 52000}, {"QCOM", 17500}, {"AVGO", 140000},
  };
  cfg.tickers.clear();
  cfg.midPrices.clear();
  cfg.tickers.reserve(opts.numTickers);
  for (std::size_t i = 0; i < opts.numTickers; ++i) {
    if (i < presets.size()) {
      cfg.tickers.push_back(presets[i].first);
      cfg.midPrices[presets[i].first] = presets[i].second;
    } else {
      const std::string symbol = "TICK" + std::to_string(i + 1);
      const Price mid = static_cast<Price>(10000 + 50 * (i % 200));
      cfg.tickers.push_back(symbol);
      cfg.midPrices[symbol] = mid;
    }
  }
  cfg.tickSize = 1;
  cfg.numOrders = opts.numOrders;
  cfg.initialDepthPerSide = 20;
  cfg.maxPriceOffsetTicks = 25;

  // Apply workload preset (controls limit/market/cancel mix)
  if (opts.workload == "crossing") {
    // Crossing-heavy: 60% market orders (guaranteed crossing) + 30% limit + 10% cancel
    cfg.limitRatio = 0.30;
    cfg.marketRatio = 0.60;
    cfg.cancelRatio = 0.10;
    cfg.maxPriceOffsetTicks = 5;  // stricter limits to increase crossing chance
  } else if (opts.workload == "resting") {
    // Resting-heavy: 70% limit + 10% market + 20% cancel
    cfg.limitRatio = 0.70;
    cfg.marketRatio = 0.10;
    cfg.cancelRatio = 0.20;
    cfg.maxPriceOffsetTicks = 50;  // wide limits → mostly resting
  } else {  // balanced (default)
    // Default balanced: 60% limit + 20% market + 20% cancel
    cfg.limitRatio = 0.60;
    cfg.marketRatio = 0.20;
    cfg.cancelRatio = 0.20;
  }

  // ---------- 2. Generate the order stream ----------
  OrderGenerator gen(cfg);
  auto messages = gen.generateAll();
  std::cout << "Generated " << messages.size() << " messages across "
            << cfg.tickers.size() << " tickers (seed=" << cfg.seed << ")\n";

  if (!opts.dumpOrders.empty()) {
    dumpOrdersJson(opts.dumpOrders, messages);
    std::cout << "  -> wrote " << opts.dumpOrders << "\n";
  }

  // ---------- 3. Feed it through the matching engine ----------
  auto t0 = std::chrono::steady_clock::now();
  std::vector<Trade> trades;

  if (opts.engine == EngineKind::SEQUENTIAL) {
    MatchingEngine eng;
    trades = eng.processAll(messages);
    auto t1 = std::chrono::steady_clock::now();

    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double msgPerSec =
        micros > 0 ? (1e6 * static_cast<double>(messages.size()) / micros) : 0.0;

    std::cout << "Engine: sequential (workload=" << opts.workload << ")\n";
    std::cout << "Processed in " << micros << " us  (~"
              << static_cast<long long>(msgPerSec) << " msgs/sec)\n";
    std::cout << "Trades produced: " << trades.size() << "\n\n";

    // ---------- 4. Per-ticker book state ----------
    std::cout << "Final book state:\n";
    for (const auto& ticker : cfg.tickers) {
      printBookSummary(eng, ticker);
    }

    // ---------- 5. Sample a few trades as a sanity check ----------
    if (!trades.empty()) {
      std::cout << "\nFirst 5 trades:\n";
      for (std::size_t i = 0; i < std::min<std::size_t>(5, trades.size()); ++i) {
        const auto& t = trades[i];
        std::cout << "  " << t.ticker
                  << "  buy=" << t.buyOrderId
                  << " sell=" << t.sellOrderId
                  << " price=" << t.price
                  << " qty=" << t.quantity << "\n";
      }
    }

    // ---------- 6. Optional dumps for the golden-trace harness ----------
    if (!opts.dumpTrades.empty()) {
      dumpTradesJson(opts.dumpTrades, trades);
      std::cout << "\n  -> wrote " << opts.dumpTrades << "\n";
    }
    if (!opts.dumpBooks.empty()) {
      dumpBooksJson(opts.dumpBooks, eng, cfg.tickers);
      std::cout << "  -> wrote " << opts.dumpBooks << "\n";
    }
    return 0;
  } else if (opts.engine == EngineKind::COARSE) {
    CoarseGrainedMatchingEngine coarseEng;
    if (opts.parallel) {
      trades = coarseEng.processAllParallel(messages, opts.threads);
    } else {
      trades = coarseEng.processAll(messages);
    }
    auto t1 = std::chrono::steady_clock::now();

    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double msgPerSec = micros > 0 ? (1e6 * static_cast<double>(messages.size()) / micros) : 0.0;

    std::cout << "Engine: coarse"
              << (opts.parallel ? " (parallel by ticker" : " (single-threaded")
              << ", workload=" << opts.workload << ")\n";
    if (opts.parallel) {
      std::size_t tc = opts.threads;
      std::cout << "Thread budget: " << tc << " (capped by ticker count)\n";
    }
    std::cout << "Processed in " << micros << " us  (~"
              << static_cast<long long>(msgPerSec) << " msgs/sec)\n";
    std::cout << "Trades produced: " << trades.size() << "\n\n";

    // ---------- 4. Per-ticker book state ----------
    std::cout << "Final book state:\n";
    for (const auto& ticker : cfg.tickers) {
      printBookSummary(coarseEng, ticker);
    }

    // ---------- 5. Sample a few trades as a sanity check ----------
    if (!trades.empty()) {
      std::cout << "\nFirst 5 trades:\n";
      for (std::size_t i = 0; i < std::min<std::size_t>(5, trades.size()); ++i) {
        const auto& t = trades[i];
        std::cout << "  " << t.ticker
                  << "  buy=" << t.buyOrderId
                  << " sell=" << t.sellOrderId
                  << " price=" << t.price
                  << " qty=" << t.quantity << "\n";
      }
    }

    // ---------- 6. Optional dumps for the golden-trace harness ----------
    if (!opts.dumpTrades.empty()) {
      dumpTradesJson(opts.dumpTrades, trades);
      std::cout << "\n  -> wrote " << opts.dumpTrades << "\n";
    }
    if (!opts.dumpBooks.empty()) {
      dumpBooksJson(opts.dumpBooks, coarseEng, cfg.tickers);
      std::cout << "  -> wrote " << opts.dumpBooks << "\n";
    }

    return 0;
  } else if (opts.engine == EngineKind::FINE) {
    FineGrainedMatchingEngine fineEng;
    if (opts.parallel) {
      trades = fineEng.processAllParallel(messages, opts.threads);
    } else {
      trades = fineEng.processAll(messages);
    }
    auto t1 = std::chrono::steady_clock::now();

    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double msgPerSec = micros > 0 ? (1e6 * static_cast<double>(messages.size()) / micros) : 0.0;

    std::cout << "Engine: fine-grained"
              << (opts.parallel ? " (parallel by ticker" : " (single-threaded")
              << ", workload=" << opts.workload << ")\n";
    if (opts.parallel) {
      std::size_t tc = opts.threads;
      std::cout << "Thread budget: " << tc << " (capped by ticker count)\n";
    }
    std::cout << "Processed in " << micros << " us  (~"
              << static_cast<long long>(msgPerSec) << " msgs/sec)\n";
    std::cout << "Trades produced: " << trades.size() << "\n\n";

    // ---------- 4. Per-ticker book state ----------
    std::cout << "Final book state:\n";
    for (const auto& ticker : cfg.tickers) {
      printBookSummary(fineEng, ticker);
    }

    // ---------- 5. Sample a few trades as a sanity check ----------
    if (!trades.empty()) {
      std::cout << "\nFirst 5 trades:\n";
      for (std::size_t i = 0; i < std::min<std::size_t>(5, trades.size()); ++i) {
        const auto& t = trades[i];
        std::cout << "  " << t.ticker
                  << "  buy=" << t.buyOrderId
                  << " sell=" << t.sellOrderId
                  << " price=" << t.price
                  << " qty=" << t.quantity << "\n";
      }
    }

    // ---------- 6. Optional dumps for the golden-trace harness ----------
    if (!opts.dumpTrades.empty()) {
      dumpTradesJson(opts.dumpTrades, trades);
      std::cout << "\n  -> wrote " << opts.dumpTrades << "\n";
    }
    if (!opts.dumpBooks.empty()) {
      dumpBooksJson(opts.dumpBooks, fineEng, cfg.tickers);
      std::cout << "  -> wrote " << opts.dumpBooks << "\n";
    }

    return 0;
  }

  std::cerr << "error: unknown engine kind \n";
  return 1;
}
