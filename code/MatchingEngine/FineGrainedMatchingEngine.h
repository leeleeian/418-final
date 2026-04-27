#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../LimitOrderBook/FineGrainedLimitOrderBook.h"
#include "../OrderGenerator/OrderGenerator.h"

/** Multi-ticker engine using a FineGrainedLimitOrderBook per symbol.
 *
 * Fine-grained locking: per-price-level locks for matching, per-side locks for
 * level lookup/create, global id-index lock for cancel operations.
 *
 * `processAllParallel` partitions the input by ticker (preserving per-ticker
 * message order) and processes shards with a POSIX pthread worker pool.
 * Per-level locking allows concurrent matching on different price levels.
 */
class FineGrainedMatchingEngine {
  friend void* fineMatchingPthreadWorker(void* opaque);

public:
  std::vector<Trade> onMessage(const OrderMessage& msg);

  std::vector<Trade> processAll(const std::vector<OrderMessage>& msgs);

  std::vector<Trade> processAllParallel(const std::vector<OrderMessage>& msgs,
                                        std::size_t numThreads = 1);

  const FineGrainedLimitOrderBook* bookFor(const std::string& ticker) const;

private:
  std::vector<Trade> dispatchOnBook(FineGrainedLimitOrderBook& book, const OrderMessage& msg);

  std::vector<Trade> drainShard(const std::vector<OrderMessage>& msgs,
                                const std::vector<std::size_t>& shardIndices);

  FineGrainedLimitOrderBook& bookForMut(const std::string& ticker);

  mutable std::mutex booksMapMutex_;
  std::unordered_map<std::string, std::unique_ptr<FineGrainedLimitOrderBook>> books_;
};
