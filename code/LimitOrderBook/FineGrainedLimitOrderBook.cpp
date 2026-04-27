#include "FineGrainedLimitOrderBook.h"

#include <algorithm>
#include <limits>

std::vector<Trade> FineGrainedLimitOrderBook::addLimitOrder(Id id, Side side, Price price,
                                                            Quantity quantity) {
  auto incoming = std::make_shared<Order>(id, side, price, quantity);

  // non-crossing add
  {
    // TODO(fine-grained-crossing): sharedOp can be dropped when crossing paths
    // stop depending on unique opMutex_ and use only local locks.
    std::shared_lock<std::shared_mutex> sharedOp(opMutex_);
    if (!isCrossing(side, price)) {
      rest(incoming);
      return {};
    }
  }

  // TODO: crossing add, modify this to be fine grained
  std::unique_lock<std::shared_mutex> uniqueOp(opMutex_);
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

std::vector<Trade> FineGrainedLimitOrderBook::addMarketOrder(Id id, Side side, Quantity quantity) {
  // TODO: crossing add, modify this to be fine grained
  std::unique_lock<std::shared_mutex> uniqueOp(opMutex_);
  auto incoming = std::make_shared<Order>(id, side, 0, quantity);
  std::vector<Trade> trades;

  if (side == Side::BUY) {
    matchBuyAgainstAsks(incoming, std::numeric_limits<Price>::max(), trades);
  } else {
    matchSellAgainstBids(incoming, 0, trades);
  }
  return trades;
}

void FineGrainedLimitOrderBook::cancelOrder(Id id) {
  // TODO(fine-grained-crossing): once crossing/matching no longer rely on
  // unique opMutex_, evaluate removing this shared op gate.
  std::shared_lock<std::shared_mutex> sharedOp(opMutex_);

  OrderPointer order;
  {
    std::lock_guard<std::mutex> ordersLock(ordersMutex_);
    auto orderIt = orders_.find(id);
    if (orderIt == orders_.end()) {
      return;
    }
    order = orderIt->second;
  }

  auto& sideMap = (order->getSide() == Side::BUY) ? bids_ : asks_;
  std::mutex& sideMutex = (order->getSide() == Side::BUY) ? bidsMutex_ : asksMutex_;
  std::lock_guard<std::mutex> sideLock(sideMutex);
  auto levelIt = sideMap.find(order->getPrice());
  if (levelIt == sideMap.end()) {
    return;
  }

  PriceLevel& level = *levelIt->second;
  {
    std::lock_guard<std::mutex> levelLock(level.levelMutex);
    auto iterIt = level.orderIters.find(id);
    if (iterIt != level.orderIters.end()) {
      level.orders.erase(iterIt->second);
      level.orderIters.erase(iterIt);
    }
  }
  if (level.orders.empty()) {
    sideMap.erase(levelIt);
  }
  std::lock_guard<std::mutex> ordersLock(ordersMutex_);
  orders_.erase(id);
}

std::vector<Trade> FineGrainedLimitOrderBook::modifyOrder(Id id, Price newPrice, Quantity newQuantity) {
  std::unique_lock<std::shared_mutex> uniqueOp(opMutex_);
  auto orderIt = orders_.find(id);
  if (orderIt == orders_.end()) {
    return {};
  }
  const Side side = orderIt->second->getSide();

  // cancel the order
  OrderPointer order = orderIt->second;
  auto& sideMap = (order->getSide() == Side::BUY) ? bids_ : asks_;
  auto levelIt = sideMap.find(order->getPrice());
  if (levelIt != sideMap.end()) {
    PriceLevel& level = *levelIt->second;
    {
      std::lock_guard<std::mutex> levelLock(level.levelMutex);
      auto iterIt = level.orderIters.find(id);
      if (iterIt != level.orderIters.end()) {
        level.orders.erase(iterIt->second);
        level.orderIters.erase(iterIt);
      }
    }
    if (level.orders.empty()) {
      sideMap.erase(levelIt);
    }
  }
  orders_.erase(orderIt);

  // add the new order
  auto incoming = std::make_shared<Order>(id, side, newPrice, newQuantity);
  std::vector<Trade> trades;
  if (side == Side::BUY) {
    matchBuyAgainstAsks(incoming, newPrice, trades);
  } else {
    matchSellAgainstBids(incoming, newPrice, trades);
  }
  if (incoming->getRemainingQuantity() > 0) {
    auto& restSideMap = (incoming->getSide() == Side::BUY) ? bids_ : asks_;
    const Price p = incoming->getPrice();
    PriceLevelPointer& levelPtr = restSideMap[p];
    if (!levelPtr) {
      levelPtr = std::make_shared<PriceLevel>();
      levelPtr->price = p;
    }
    PriceLevel& level = *levelPtr;
    std::lock_guard<std::mutex> levelLock(level.levelMutex);
    level.orders.push_back(incoming);
    level.orderIters[incoming->getId()] = std::prev(level.orders.end());
    orders_[incoming->getId()] = incoming;
  }
  return trades;
}

bool FineGrainedLimitOrderBook::hasOrder(Id id) const {
  std::lock_guard<std::mutex> ordersLock(ordersMutex_);
  return orders_.find(id) != orders_.end();
}

std::size_t FineGrainedLimitOrderBook::restingOrderCount() const {
  std::lock_guard<std::mutex> ordersLock(ordersMutex_);
  return orders_.size();
}

std::size_t FineGrainedLimitOrderBook::bidLevelCount() const {
  std::lock_guard<std::mutex> sideLock(bidsMutex_);
  return bids_.size();
}

std::size_t FineGrainedLimitOrderBook::askLevelCount() const {
  std::lock_guard<std::mutex> sideLock(asksMutex_);
  return asks_.size();
}

BookSnapshot FineGrainedLimitOrderBook::snapshot() const {
  std::shared_lock<std::shared_mutex> sharedOp(opMutex_);
  BookSnapshot snap;

  auto copySide = [](const std::map<Price, PriceLevelPointer>& sideMap,
                     std::vector<BookSnapshot::LevelView>& out) {
    out.reserve(sideMap.size());
    for (const auto& kv : sideMap) {
      const PriceLevel& level = *kv.second;
      BookSnapshot::LevelView lv;
      lv.price = kv.first;
      {
        std::lock_guard<std::mutex> levelLock(level.levelMutex);
        lv.orders.reserve(level.orders.size());
        for (const auto& orderPtr : level.orders) {
          lv.orders.push_back({orderPtr->getId(), orderPtr->getRemainingQuantity()});
        }
      }
      out.push_back(std::move(lv));
    }
  };

  {
    std::lock_guard<std::mutex> bidSideLock(bidsMutex_);
    copySide(bids_, snap.bids);
  }
  {
    std::lock_guard<std::mutex> askSideLock(asksMutex_);
    copySide(asks_, snap.asks);
  }
  return snap;
}

void FineGrainedLimitOrderBook::matchBuyAgainstAsks(const OrderPointer& incoming, Price maxPrice,
                                                    std::vector<Trade>& trades) {
  while (incoming->getRemainingQuantity() > 0 && !asks_.empty()) {
    auto bestIt = asks_.begin();
    if (bestIt->first > maxPrice) {
      break;
    }

    PriceLevel& level = *bestIt->second;
    std::lock_guard<std::mutex> levelLock(level.levelMutex);
    OrderPointer resting = level.orders.front();

    const Quantity tradeQty = std::min(incoming->getRemainingQuantity(), resting->getRemainingQuantity());
    incoming->fill(tradeQty);
    resting->fill(tradeQty);
    trades.push_back({incoming->getId(), resting->getId(), level.price, tradeQty, {}});

    if (resting->getRemainingQuantity() == 0) {
      const Id rid = resting->getId();
      level.orderIters.erase(rid);
      level.orders.pop_front();
      orders_.erase(rid);
    }
    if (level.orders.empty()) {
      asks_.erase(bestIt);
    }
  }
}

void FineGrainedLimitOrderBook::matchSellAgainstBids(const OrderPointer& incoming, Price minPrice,
                                                     std::vector<Trade>& trades) {
  while (incoming->getRemainingQuantity() > 0 && !bids_.empty()) {
    auto bestIt = std::prev(bids_.end());
    if (bestIt->first < minPrice) {
      break;
    }

    PriceLevel& level = *bestIt->second;
    std::lock_guard<std::mutex> levelLock(level.levelMutex);
    OrderPointer resting = level.orders.front();

    const Quantity tradeQty = std::min(incoming->getRemainingQuantity(), resting->getRemainingQuantity());
    incoming->fill(tradeQty);
    resting->fill(tradeQty);
    trades.push_back({resting->getId(), incoming->getId(), level.price, tradeQty, {}});

    if (resting->getRemainingQuantity() == 0) {
      const Id rid = resting->getId();
      level.orderIters.erase(rid);
      level.orders.pop_front();
      orders_.erase(rid);
    }
    if (level.orders.empty()) {
      bids_.erase(bestIt);
    }
  }
}

void FineGrainedLimitOrderBook::rest(const OrderPointer& order) {
  auto& sideMap = (order->getSide() == Side::BUY) ? bids_ : asks_;
  std::mutex& sideMutex = (order->getSide() == Side::BUY) ? bidsMutex_ : asksMutex_;
  const Price p = order->getPrice();
  PriceLevelPointer levelPtr;
  {
    std::lock_guard<std::mutex> sideLock(sideMutex);
    PriceLevelPointer& slot = sideMap[p];
    if (!slot) {
      slot = std::make_shared<PriceLevel>();
      slot->price = p;
    }
    levelPtr = slot;
  }
  PriceLevel& level = *levelPtr;

  std::lock_guard<std::mutex> levelLock(level.levelMutex);
  level.orders.push_back(order);
  level.orderIters[order->getId()] = std::prev(level.orders.end());
  std::lock_guard<std::mutex> ordersLock(ordersMutex_);
  orders_[order->getId()] = order;
}

bool FineGrainedLimitOrderBook::isCrossing(Side side, Price price) const {
  if (side == Side::BUY) {
    std::lock_guard<std::mutex> askSideLock(asksMutex_);
    if (asks_.empty()) {
      return false;
    }
    return asks_.begin()->first <= price;
  }
  std::lock_guard<std::mutex> bidSideLock(bidsMutex_);
  if (bids_.empty()) {
    return false;
  }
  return std::prev(bids_.end())->first >= price;
}
