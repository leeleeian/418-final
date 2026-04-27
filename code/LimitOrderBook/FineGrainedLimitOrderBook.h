#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
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

  mutable std::recursive_mutex bookMutex_;
  std::map<Price, PriceLevelPointer> bids_;
  std::map<Price, PriceLevelPointer> asks_;
  std::unordered_map<Id, OrderPointer> orders_;
};
