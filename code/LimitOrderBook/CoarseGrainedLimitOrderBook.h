#pragma once

#include <mutex>
#include <vector>

#include "LimitOrderBook.h"

/** Thread-safe wrapper for LimitOrderBook: one mutex serializes all access to an underlying
 * LimitOrderBook. Different tickers use different instances, so matching across symbols can 
 * proceed in parallel when driven from separate threads.
 */
class CoarseGrainedLimitOrderBook {
public:
  std::vector<Trade> addLimitOrder(Id id, Side side, Price price, Quantity quantity);
  std::vector<Trade> addMarketOrder(Id id, Side side, Quantity quantity);
  void cancelOrder(Id id);
  std::vector<Trade> modifyOrder(Id id, Price newPrice, Quantity newQuantity);

  bool hasOrder(Id id) const;
  std::size_t restingOrderCount() const;
  std::size_t bidLevelCount() const;
  std::size_t askLevelCount() const;
  BookSnapshot snapshot() const;

private:
  mutable std::mutex mutex_;
  LimitOrderBook book_;
};
