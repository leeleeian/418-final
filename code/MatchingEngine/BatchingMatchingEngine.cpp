#include "BatchingMatchingEngine.h"

#include <algorithm>
#include <memory>

std::shared_ptr<Order> BatchingMatchingEngine::makeOrder(const OrderMessage& msg) {
  return std::make_shared<Order>(msg.orderId, msg.side, msg.price, msg.quantity);
}

std::vector<Trade> BatchingMatchingEngine::drainShard(
    const std::vector<OrderMessage>& msgs,
    const std::vector<std::size_t>& shardIndices) {
  if (shardIndices.empty()) {
    return {};
  }

  CoarseGrainedLimitOrderBook& book = bookForMut(msgs[shardIndices.front()].ticker);
  std::vector<Trade> local;
  local.reserve(shardIndices.size());

  size_t i = 0;
  while (i < shardIndices.size()) {
    const OrderMessage& msg = msgs[shardIndices[i]];

    // Detect run of consecutive non-crossing limit orders
    bool isBatchable = (msg.action == ActionType::NEW &&
                        msg.orderType == OrderType::LIMIT &&
                        !book.wouldCross(msg.side, msg.price));

    if (isBatchable) {
      // Accumulate consecutive non-crossing limit orders
      std::vector<std::shared_ptr<Order>> batch;
      while (i < shardIndices.size()) {
        const OrderMessage& m = msgs[shardIndices[i]];
        if (m.action != ActionType::NEW ||
            m.orderType != OrderType::LIMIT ||
            book.wouldCross(m.side, m.price)) {
          break;  // Stop batch at first active order
        }
        batch.push_back(makeOrder(m));
        i++;
      }
      // Batch insert all non-crossing orders at once
      book.batchRest(std::move(batch));
      // Non-crossing orders produce no trades
    } else {
      // Process market/cancel/crossing orders normally
      auto trades = dispatchOnBook(book, msg);
      local.insert(local.end(), std::make_move_iterator(trades.begin()),
                   std::make_move_iterator(trades.end()));
      i++;
    }
  }

  return local;
}
