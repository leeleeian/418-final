#pragma once

#include "CoarseGrainedMatchingEngine.h"

/** Batching-optimized matching engine for non-crossing limit orders.
 *
 * Extends CoarseGrainedMatchingEngine with batching logic in drainShard:
 * consecutive non-crossing limit orders are accumulated and inserted as a batch,
 * reducing lock acquisitions compared to per-order insertion.
 *
 * Preserves all ordering guarantees: per-ticker message order and FIFO
 * within price levels.
 */
class BatchingMatchingEngine : public CoarseGrainedMatchingEngine {
public:
  // Override drainShard to batch non-crossing limit orders
  std::vector<Trade> drainShard(const std::vector<OrderMessage>& msgs,
                                const std::vector<std::size_t>& shardIndices);

private:
  // Helper: Build an Order from an OrderMessage (shared utility)
  static std::shared_ptr<Order> makeOrder(const OrderMessage& msg);
};
