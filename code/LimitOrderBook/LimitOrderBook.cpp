#include "LimitOrderBook.h"

#include <algorithm>
#include <limits>

std::vector<Trade> LimitOrderBook::addLimitOrder(Id id, Side side, Price price,
     Quantity quantity) {
  auto incoming = std::make_shared<Order>(id, side, price, quantity);
  std::vector<Trade> trades;

  if (side == Side::BUY) {
    matchBuyAgainstAsks(incoming, price, trades);
  } else {
    matchSellAgainstBids(incoming, price, trades);
  }

  if (incoming->getRemainingQuantity() > 0) {
    rest(incoming);
  }
  return trades;
}

std::vector<Trade> LimitOrderBook::addMarketOrder(Id id, Side side, Quantity quantity) {
  // Price field is unused for market orders
  auto incoming = std::make_shared<Order>(id, side, 0, quantity);
  std::vector<Trade> trades;

  if (side == Side::BUY) {
    // Cross at any ask price.
    matchBuyAgainstAsks(incoming, std::numeric_limits<Price>::max(), trades);
  } else {
    // Cross at any bid price then min price = 0
    matchSellAgainstBids(incoming, 0, trades);
  }
  // Any unfilled remainder is dropped — market orders don't stay
  return trades;
}

void LimitOrderBook::cancelOrder(Id id) {
  auto orderIt = orders_.find(id);
  if (orderIt == orders_.end()) return;  // already filled or never existed

  OrderPointer order = orderIt->second;
  auto& sideMap = (order->getSide() == Side::BUY) ? bids_ : asks_;

  auto levelIt = sideMap.find(order->getPrice());
  if (levelIt != sideMap.end()) {
    PriceLevel& level = *levelIt->second;
    auto iterIt = level.orderIters.find(id);
    if (iterIt != level.orderIters.end()) {
      level.orders.erase(iterIt->second);
      level.orderIters.erase(iterIt);
    }
    if (level.orders.empty()) {
      sideMap.erase(levelIt);
    }
  }

  orders_.erase(orderIt);
}

std::vector<Trade> LimitOrderBook::modifyOrder(Id id, Price newPrice, Quantity newQuantity) {
  auto orderIt = orders_.find(id);
  if (orderIt == orders_.end()) return {};

  Side side = orderIt->second->getSide();
  cancelOrder(id);
  // Modified order may now cross — re-route through the matcher.
  return addLimitOrder(id, side, newPrice, newQuantity);
}

bool LimitOrderBook::hasOrder(Id id) const {
  return orders_.find(id) != orders_.end();
}

std::size_t LimitOrderBook::restingOrderCount() const { return orders_.size(); }
std::size_t LimitOrderBook::bidLevelCount() const { return bids_.size(); }
std::size_t LimitOrderBook::askLevelCount() const { return asks_.size(); }

BookSnapshot LimitOrderBook::snapshot() const {
  BookSnapshot snap;

  auto copySide = [](const std::map<Price, PriceLevelPointer>& sideMap,
                     std::vector<BookSnapshot::LevelView>& out) {
    out.reserve(sideMap.size());
    // std::map iterates ascending, which is what BookSnapshot guarantees.
    for (const auto& kv : sideMap) {
      const PriceLevel& level = *kv.second;
      BookSnapshot::LevelView lv;
      lv.price = kv.first;
      lv.orders.reserve(level.orders.size());
      // std::list iterates front-to-back == FIFO order.
      for (const auto& orderPtr : level.orders) {
        lv.orders.push_back({orderPtr->getId(), orderPtr->getRemainingQuantity()});
      }
      out.push_back(std::move(lv));
    }
  };

  copySide(bids_, snap.bids);
  copySide(asks_, snap.asks);
  return snap;
}


/* ------ private helpers  ----- */

void LimitOrderBook::matchBuyAgainstAsks(const OrderPointer& incoming, Price maxPrice,
      std::vector<Trade>& trades) {
  while (incoming->getRemainingQuantity() > 0 && !asks_.empty()) {
    auto bestIt = asks_.begin();  // lowest ask
    if (bestIt->first > maxPrice) break;

    PriceLevel& level = *bestIt->second;
    OrderPointer resting = level.orders.front();  // earliest at this level

    Quantity tradeQty = std::min(incoming->getRemainingQuantity(),
                                 resting->getRemainingQuantity());
    incoming->fill(tradeQty);
    resting->fill(tradeQty);
    // Trade prints at the resting (passive) order's price.
    // ticker is left empty here; MatchingEngine stamps it after the call.
    trades.push_back({incoming->getId(), resting->getId(), level.price, tradeQty, {}});

    if (resting->getRemainingQuantity() == 0) {
      Id rid = resting->getId();
      level.orderIters.erase(rid);
      level.orders.pop_front();
      orders_.erase(rid);
    }
    if (level.orders.empty()) {
      asks_.erase(bestIt);
    }
  }
}

void LimitOrderBook::matchSellAgainstBids(const OrderPointer& incoming, Price minPrice,
      std::vector<Trade>& trades) {
  while (incoming->getRemainingQuantity() > 0 && !bids_.empty()) {
    auto bestIt = std::prev(bids_.end());  // highest bid
    if (bestIt->first < minPrice) break;

    PriceLevel& level = *bestIt->second;
    OrderPointer resting = level.orders.front();

    Quantity tradeQty = std::min(incoming->getRemainingQuantity(),
                                 resting->getRemainingQuantity());
    incoming->fill(tradeQty);
    resting->fill(tradeQty);
    trades.push_back({resting->getId(), incoming->getId(), level.price, tradeQty, {}});

    if (resting->getRemainingQuantity() == 0) {
      Id rid = resting->getId();
      level.orderIters.erase(rid);
      level.orders.pop_front();
      orders_.erase(rid);
    }
    if (level.orders.empty()) {
      bids_.erase(bestIt);
    }
  }
}

void LimitOrderBook::rest(const OrderPointer& order) {
  auto& sideMap = (order->getSide() == Side::BUY) ? bids_ : asks_;
  Price p = order->getPrice();

  PriceLevelPointer& levelPtr = sideMap[p];
  if (!levelPtr) {
    levelPtr = std::make_shared<PriceLevel>();
    levelPtr->price = p;
  }
  PriceLevel& level = *levelPtr;

  level.orders.push_back(order);
  level.orderIters[order->getId()] = std::prev(level.orders.end());
  orders_[order->getId()] = order;
}
