#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../LimitOrderBook/LimitOrderBook.h"
#include "../OrderGenerator/OrderGenerator.h"

/** Stateless front-end that turns OrderMessages into book operations.
 *
 * Owns one LimitOrderBook per ticker, lazily created on first use. Trades
 * produced by each message are returned to the caller — the engine itself
 * does not maintain a tape.
 *
 * Single-threaded for v1. The threaded command-queue / batching layer
 * described in the spec is intended to wrap this class, not live inside it.
 */
class MatchingEngine {
public:
  // Apply a single message and return any trades it produced.
  std::vector<Trade> onMessage(const OrderMessage& msg);

  // Apply a batch of messages in arrival order, concatenating their trades.
  std::vector<Trade> processAll(const std::vector<OrderMessage>& msgs);

  // Read-only access to a per-ticker book (nullptr if the ticker is unknown).
  const LimitOrderBook* bookFor(const std::string& ticker) const;

private:
  LimitOrderBook& bookForMut(const std::string& ticker);

  // Apply dispatch logic after the per-ticker book is resolved (`onMessage` uses this).
  std::vector<Trade> applyOnBook(LimitOrderBook& book, const OrderMessage& msg);

  std::unordered_map<std::string, std::unique_ptr<LimitOrderBook>> books_;
};
