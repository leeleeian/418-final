#include "FineGrainedMatchingEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <stdexcept>

std::vector<Trade> FineGrainedMatchingEngine::dispatchOnBook(FineGrainedLimitOrderBook& book,
                                                              const OrderMessage& msg) {
  std::vector<Trade> trades;
  switch (msg.action) {
    case ActionType::NEW:
      switch (msg.orderType) {
        case OrderType::LIMIT:
          trades = book.addLimitOrder(msg.orderId, msg.side, msg.price, msg.quantity);
          break;
        case OrderType::MARKET:
          trades = book.addMarketOrder(msg.orderId, msg.side, msg.quantity);
          break;
      }
      break;

    case ActionType::CANCEL:
      book.cancelOrder(msg.orderId);
      return {};
  }

  for (auto& t : trades) {
    t.ticker = msg.ticker;
  }
  return trades;
}

std::vector<Trade> FineGrainedMatchingEngine::onMessage(const OrderMessage& msg) {
  FineGrainedLimitOrderBook& book = bookForMut(msg.ticker);
  return dispatchOnBook(book, msg);
}

std::vector<Trade> FineGrainedMatchingEngine::processAll(
    const std::vector<OrderMessage>& msgs) {
  std::vector<Trade> all;
  all.reserve(msgs.size());
  for (const auto& msg : msgs) {
    auto trades = onMessage(msg);
    all.insert(all.end(),
               std::make_move_iterator(trades.begin()),
               std::make_move_iterator(trades.end()));
  }
  return all;
}

std::vector<Trade> FineGrainedMatchingEngine::drainShard(
    const std::vector<OrderMessage>& msgs,
    const std::vector<std::size_t>& shardIndices) {
  if (shardIndices.empty()) {
    return {};
  }
  FineGrainedLimitOrderBook& book = bookForMut(msgs[shardIndices.front()].ticker);
  std::vector<Trade> local;
  local.reserve(shardIndices.size());
  for (std::size_t idx : shardIndices) {
    auto trades = dispatchOnBook(book, msgs[idx]);
    local.insert(local.end(), std::make_move_iterator(trades.begin()),
                std::make_move_iterator(trades.end()));
  }
  return local;
}

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t microsBetween(Clock::time_point start, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

bool fineBreakdownEnabled() {
  const char* env = std::getenv("LOB_FINE_BREAKDOWN");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

void printRegion(const char* label, std::uint64_t us, std::uint64_t totalUs) {
  const double pct = totalUs == 0 ? 0.0 : (100.0 * static_cast<double>(us) / totalUs);
  std::cout << "  - " << label << ": " << us << " us (" << pct << "%)\n";
}

struct PthreadShard {
  FineGrainedMatchingEngine* engine;
  const std::vector<OrderMessage>* msgs;
  const std::vector<const std::vector<std::size_t>*>* shards;
  std::atomic<std::size_t>* next;
  std::mutex* mergeMutex;
  std::vector<Trade>* all;
  std::atomic<std::uint64_t>* drainMicros;
  std::atomic<std::uint64_t>* mergeMicros;
};

} // namespace

//  `pthread_create` entry: claim shard indices with `fetch_add`, drain each shard,
//   append trades into `*all` under `mergeMutex`. Stops when index >= shard count.

void* fineMatchingPthreadWorker(void* opaque) {
  auto* body = static_cast<PthreadShard*>(opaque);
  for (;;) {
    std::size_t i = body->next->fetch_add(1, std::memory_order_relaxed);
    if (i >= body->shards->size()) {
      break;
    }
    const auto drainStart = Clock::now();
    std::vector<Trade> local =
        body->engine->drainShard(*body->msgs, *(*body->shards)[i]);
    const auto drainEnd = Clock::now();
    body->drainMicros->fetch_add(microsBetween(drainStart, drainEnd), std::memory_order_relaxed);

    const auto mergeStart = Clock::now();
    std::lock_guard<std::mutex> lock(*body->mergeMutex);
    body->all->insert(body->all->end(), std::make_move_iterator(local.begin()),
                   std::make_move_iterator(local.end()));
    const auto mergeEnd = Clock::now();
    body->mergeMicros->fetch_add(microsBetween(mergeStart, mergeEnd), std::memory_order_relaxed);
  }
  return nullptr;
}

std::vector<Trade> FineGrainedMatchingEngine::processAllParallel(
    const std::vector<OrderMessage>& msgs, std::size_t numThreads) {

  const bool emitBreakdown = fineBreakdownEnabled();
  const auto startTotal = Clock::now();
  if (msgs.empty()) return {};

  const auto partitionStart = Clock::now();
  std::unordered_map<std::string, std::vector<std::size_t>> byTicker;
  byTicker.reserve(32);
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    byTicker[msgs[i].ticker].push_back(i);
  }

  std::vector<const std::vector<std::size_t>*> shards;
  shards.reserve(byTicker.size());
  for (const auto& kv : byTicker) { shards.push_back(&kv.second); }
  if (shards.empty()) { return {}; }
  const auto partitionEnd = Clock::now();

  std::size_t tc = numThreads;
  tc = std::max<std::size_t>(1, std::min(tc, shards.size()));

  std::vector<Trade> allTrades;
  allTrades.reserve(msgs.size());

  if (tc == 1) {
    const auto workerStart = Clock::now();
    for (const auto* shard : shards) {
      auto local = drainShard(msgs, *shard);
      allTrades.insert(allTrades.end(), std::make_move_iterator(local.begin()),
                 std::make_move_iterator(local.end()));
    }
    const auto workerEnd = Clock::now();
    if (emitBreakdown) {
      const auto totalUs = microsBetween(startTotal, workerEnd);
      const auto partitionUs = microsBetween(partitionStart, partitionEnd);
      const auto workerUs = microsBetween(workerStart, workerEnd);
      std::cout << "[fine-breakdown] processAllParallel threads=1\n";
      printRegion("partition by ticker", partitionUs, totalUs);
      printRegion("drain + merge (single worker)", workerUs, totalUs);
      printRegion("other overhead", totalUs - partitionUs - workerUs, totalUs);
      std::cout << "  total: " << totalUs << " us\n";
    }
    return allTrades;
  }

  std::atomic<std::uint64_t> drainMicros{0};
  std::atomic<std::uint64_t> mergeMicros{0};
  std::atomic<std::size_t> next{0};
  std::mutex mergeMutex;
  PthreadShard body{this, &msgs, &shards, &next, &mergeMutex, &allTrades,
                    &drainMicros, &mergeMicros};
  std::vector<pthread_t> workers(tc);
  const auto launchStart = Clock::now();
  for (std::size_t w = 0; w < tc; ++w) {
    if (pthread_create(&workers[w], nullptr, fineMatchingPthreadWorker, &body) != 0) {
      for (std::size_t j = 0; j < w; ++j) {
        pthread_join(workers[j], nullptr);
      }
      throw std::runtime_error("pthread_create failed");
    }
  }
  const auto launchEnd = Clock::now();
  const auto joinStart = Clock::now();
  for (std::size_t w = 0; w < tc; ++w) {
    pthread_join(workers[w], nullptr);
  }
  const auto endTotal = Clock::now();

  if (emitBreakdown) {
    const auto totalUs = microsBetween(startTotal, endTotal);
    const auto partitionUs = microsBetween(partitionStart, partitionEnd);
    const auto launchUs = microsBetween(launchStart, launchEnd);
    const auto joinUs = microsBetween(joinStart, endTotal);
    const auto workerWallUs = microsBetween(launchEnd, endTotal);
    const auto drainAggUs = drainMicros.load(std::memory_order_relaxed);
    const auto mergeAggUs = mergeMicros.load(std::memory_order_relaxed);
    const std::uint64_t accounted = partitionUs + launchUs + joinUs;

    std::cout << "[fine-breakdown] processAllParallel threads=" << tc << "\n";
    printRegion("partition by ticker", partitionUs, totalUs);
    printRegion("thread launch", launchUs, totalUs);
    printRegion("join/wait for workers", joinUs, totalUs);
    printRegion("other overhead", totalUs > accounted ? totalUs - accounted : 0, totalUs);
    std::cout << "  total: " << totalUs << " us\n";
    std::cout << "  worker phase wall-time: " << workerWallUs << " us\n";
    std::cout << "  aggregate worker drain time (sum over threads): " << drainAggUs << " us\n";
    std::cout << "  aggregate merge critical-section time (sum over threads): " << mergeAggUs << " us\n";
  }
  return allTrades;
}

const FineGrainedLimitOrderBook* FineGrainedMatchingEngine::bookFor(
    const std::string& ticker) const {
  std::lock_guard<std::mutex> lock(booksMapMutex_);
  auto it = books_.find(ticker);
  return it == books_.end() ? nullptr : it->second.get();
}

FineGrainedLimitOrderBook& FineGrainedMatchingEngine::bookForMut(
    const std::string& ticker) {
  std::lock_guard<std::mutex> lock(booksMapMutex_);
  auto it = books_.find(ticker);
  if (it == books_.end()) {
    it = books_.emplace(ticker, std::make_unique<FineGrainedLimitOrderBook>()).first;
  }
  return *it->second;
}
