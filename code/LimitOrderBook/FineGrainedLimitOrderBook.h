#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "LimitOrderBook.h"

class FineGrainedLimitOrderBook {
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

  // Check if a limit order at given price/side would cross (match liquidity).
  bool wouldCross(Side side, Price price) const;

  // Batch rest multiple non-crossing limit orders (caller guarantees non-crossing).
  void batchRest(std::vector<OrderPointer> orders);

private:
  struct PriceLevel {
    Price price;
    std::list<OrderPointer> orders;
    std::unordered_map<Id, std::list<OrderPointer>::iterator> orderIters;
    mutable std::mutex levelMutex;
  };
  using PriceLevelPointer = std::shared_ptr<PriceLevel>;

  void matchBuyAgainstAsks(const OrderPointer& incoming, Price maxPrice,
                           std::vector<Trade>& trades);
  void matchSellAgainstBids(const OrderPointer& incoming, Price minPrice,
                            std::vector<Trade>& trades);
  void rest(const OrderPointer& order);
  bool isCrossing(Side side, Price price) const;

  // global locks (no opMutex_ needed: hand-over-hand matching per-level)
  mutable std::mutex bidsMutex_; // side mutex for buy side
  mutable std::mutex asksMutex_; // side mutex for sell side
  mutable std::mutex ordersMutex_; // global id index lock

  // data structures
  std::map<Price, PriceLevelPointer> bids_;
  std::map<Price, PriceLevelPointer> asks_;
  std::unordered_map<Id, OrderPointer> orders_;
};
