#include "FineGrainedMatchingEngine.h"

#include <algorithm>
#include <atomic>
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

struct PthreadShard {
  FineGrainedMatchingEngine* engine;
  const std::vector<OrderMessage>* msgs;
  const std::vector<const std::vector<std::size_t>*>* shards;
  std::atomic<std::size_t>* next;
  std::mutex* mergeMutex;
  std::vector<Trade>* all;
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
    std::vector<Trade> local =
        body->engine->drainShard(*body->msgs, *(*body->shards)[i]);
    std::lock_guard<std::mutex> lock(*body->mergeMutex);
    body->all->insert(body->all->end(), std::make_move_iterator(local.begin()),
                   std::make_move_iterator(local.end()));
  }
  return nullptr;
}

std::vector<Trade> FineGrainedMatchingEngine::processAllParallel(
    const std::vector<OrderMessage>& msgs, std::size_t numThreads) {

  if (msgs.empty()) return {};

  std::unordered_map<std::string, std::vector<std::size_t>> byTicker;
  byTicker.reserve(32);
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    byTicker[msgs[i].ticker].push_back(i);
  }

  std::vector<const std::vector<std::size_t>*> shards;
  shards.reserve(byTicker.size());
  for (const auto& kv : byTicker) { shards.push_back(&kv.second); }
  if (shards.empty()) { return {}; }

  std::size_t tc = numThreads;
  tc = std::max<std::size_t>(1, std::min(tc, shards.size()));

  std::vector<Trade> allTrades;
  allTrades.reserve(msgs.size());

  if (tc == 1) {
    for (const auto* shard : shards) {
      auto local = drainShard(msgs, *shard);
      allTrades.insert(allTrades.end(), std::make_move_iterator(local.begin()),
                 std::make_move_iterator(local.end()));
    }
    return allTrades;
  }

  std::atomic<std::size_t> next{0};
  std::mutex mergeMutex;
  PthreadShard body{this, &msgs, &shards, &next, &mergeMutex, &allTrades};
  std::vector<pthread_t> workers(tc);
  for (std::size_t w = 0; w < tc; ++w) {
    if (pthread_create(&workers[w], nullptr, fineMatchingPthreadWorker, &body) != 0) {
      for (std::size_t j = 0; j < w; ++j) {
        pthread_join(workers[j], nullptr);
      }
      throw std::runtime_error("pthread_create failed");
    }
  }
  for (std::size_t w = 0; w < tc; ++w) {
    pthread_join(workers[w], nullptr);
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
