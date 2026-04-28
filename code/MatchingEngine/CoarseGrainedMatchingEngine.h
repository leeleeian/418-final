#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../LimitOrderBook/CoarseGrainedLimitOrderBook.h"
#include "../OrderGenerator/OrderGenerator.h"

/** Multi-ticker engine using a CoarseGrainedLimitOrderBook per symbol.
 *
 * A mutex protects the `books_` map; each book has its own mutex for
 * coarse-grained locking of matching state.
 *
 * `processAllParallel` partitions the input by ticker (preserving per-ticker
 * message order) and processes shards with a POSIX pthread worker pool when
 * the worker count is greater than one. Global trade order may differ from
 * sequential `processAll`; final per-ticker books should match the sequential
 * engine for the same stream.
 */
class CoarseGrainedMatchingEngine {
  friend void* coarseMatchingPthreadWorker(void* opaque);

public:
  std::vector<Trade> onMessage(const OrderMessage& msg);

  std::vector<Trade> processAll(const std::vector<OrderMessage>& msgs);

  std::vector<Trade> processAllParallel(const std::vector<OrderMessage>& msgs,
                                        std::size_t numThreads = 1);

  const CoarseGrainedLimitOrderBook* bookFor(const std::string& ticker) const;

protected:
  // Drain the messages in `msgs` whose indices appear in `shardIndices`,
  // in order. Using indices instead of a copy of the shard avoids deep-copying
  // OrderMessage (and its heap-allocated ticker string) during partitioning.
  std::vector<Trade> dispatchOnBook(CoarseGrainedLimitOrderBook& book, const OrderMessage& msg);

  std::vector<Trade> drainShard(const std::vector<OrderMessage>& msgs,
                                const std::vector<std::size_t>& shardIndices);

  CoarseGrainedLimitOrderBook& bookForMut(const std::string& ticker);

private:

  mutable std::mutex booksMapMutex_;
  std::unordered_map<std::string, std::unique_ptr<CoarseGrainedLimitOrderBook>> books_;
};
