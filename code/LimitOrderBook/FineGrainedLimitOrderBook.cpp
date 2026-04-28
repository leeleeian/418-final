#include "FineGrainedLimitOrderBook.h"

#include <algorithm>
#include <limits>

std::vector<Trade> FineGrainedLimitOrderBook::addLimitOrder(Id id, Side side, Price price,
                                                            Quantity quantity) {
  auto incoming = std::make_shared<Order>(id, side, price, quantity);

  // Check if crossing (non-blocking, snapshot check)
  if (!isCrossing(side, price)) {
    rest(incoming);
    return {};
  }

  // Crossing: hand-over-hand matching (per-level, no global lock)
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
  auto incoming = std::make_shared<Order>(id, side, 0, quantity);
  std::vector<Trade> trades;

  // Market orders always cross: hand-over-hand matching (per-level, no global lock)
  if (side == Side::BUY) {
    matchBuyAgainstAsks(incoming, std::numeric_limits<Price>::max(), trades);
  } else {
    matchSellAgainstBids(incoming, 0, trades);
  }
  return trades;
}

void FineGrainedLimitOrderBook::cancelOrder(Id id) {
  // No opMutex_ needed: crossing is now hand-over-hand fine-grained
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
  // No opMutex_ needed: crossing is now hand-over-hand fine-grained
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
  // No opMutex_ needed: per-level locks sufficient for correctness
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
  while (incoming->getRemainingQuantity() > 0) {
    Price bestPrice;
    PriceLevelPointer levelPtr;

    // Find best ask level (hand-over-hand: briefly lock side, get ptr, unlock)
    {
      std::lock_guard<std::mutex> sideLock(asksMutex_);
      if (asks_.empty()) break;
      auto bestIt = asks_.begin();
      if (bestIt->first > maxPrice) break;
      bestPrice = bestIt->first;
      levelPtr = bestIt->second;
    }

    // Match at this level (no side lock held)
    {
      std::lock_guard<std::mutex> levelLock(levelPtr->levelMutex);
      if (levelPtr->orders.empty()) continue;  // Level was drained by another thread

      OrderPointer resting = levelPtr->orders.front();
      const Quantity tradeQty = std::min(incoming->getRemainingQuantity(), resting->getRemainingQuantity());
      incoming->fill(tradeQty);
      resting->fill(tradeQty);
      trades.push_back({incoming->getId(), resting->getId(), bestPrice, tradeQty, {}});

      if (resting->getRemainingQuantity() == 0) {
        const Id rid = resting->getId();
        levelPtr->orderIters.erase(rid);
        levelPtr->orders.pop_front();
        {
          std::lock_guard<std::mutex> ordersLock(ordersMutex_);
          orders_.erase(rid);
        }
      }
    }

    // Erase level if empty (need side lock for map erase)
    {
      std::lock_guard<std::mutex> sideLock(asksMutex_);
      if (levelPtr->orders.empty()) {
        asks_.erase(bestPrice);
      }
    }
  }
}

void FineGrainedLimitOrderBook::matchSellAgainstBids(const OrderPointer& incoming, Price minPrice,
                                                     std::vector<Trade>& trades) {
  while (incoming->getRemainingQuantity() > 0) {
    Price bestPrice;
    PriceLevelPointer levelPtr;

    // Find best bid level (hand-over-hand: briefly lock side, get ptr, unlock)
    {
      std::lock_guard<std::mutex> sideLock(bidsMutex_);
      if (bids_.empty()) break;
      auto bestIt = std::prev(bids_.end());
      if (bestIt->first < minPrice) break;
      bestPrice = bestIt->first;
      levelPtr = bestIt->second;
    }

    // Match at this level (no side lock held)
    {
      std::lock_guard<std::mutex> levelLock(levelPtr->levelMutex);
      if (levelPtr->orders.empty()) continue;  // Level was drained by another thread

      OrderPointer resting = levelPtr->orders.front();
      const Quantity tradeQty = std::min(incoming->getRemainingQuantity(), resting->getRemainingQuantity());
      incoming->fill(tradeQty);
      resting->fill(tradeQty);
      trades.push_back({resting->getId(), incoming->getId(), bestPrice, tradeQty, {}});

      if (resting->getRemainingQuantity() == 0) {
        const Id rid = resting->getId();
        levelPtr->orderIters.erase(rid);
        levelPtr->orders.pop_front();
        {
          std::lock_guard<std::mutex> ordersLock(ordersMutex_);
          orders_.erase(rid);
        }
      }
    }

    // Erase level if empty (need side lock for map erase)
    {
      std::lock_guard<std::mutex> sideLock(bidsMutex_);
      if (levelPtr->orders.empty()) {
        bids_.erase(bestPrice);
      }
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

bool FineGrainedLimitOrderBook::wouldCross(Side side, Price price) const {
  return isCrossing(side, price);
}

void FineGrainedLimitOrderBook::batchRest(std::vector<OrderPointer> orders) {
  // Group by (side, price) to minimize lock acquisitions
  // while preserving FIFO order within each (side, price) group
  std::map<std::pair<int, Price>, std::vector<OrderPointer>> groups;
  for (auto& o : orders) {
    groups[{static_cast<int>(o->getSide()), o->getPrice()}].push_back(o);
  }

  // Process each (side, price) group with minimal locking
  for (auto& [key, grp] : groups) {
    Side side = static_cast<Side>(key.first);
    Price price = key.second;
    auto& sideMap = (side == Side::BUY) ? bids_ : asks_;
    std::mutex& sideMutex = (side == Side::BUY) ? bidsMutex_ : asksMutex_;

    // Get or create price level under side lock
    PriceLevelPointer levelPtr;
    {
      std::lock_guard<std::mutex> sideLock(sideMutex);
      auto& slot = sideMap[price];
      if (!slot) {
        slot = std::make_shared<PriceLevel>();
        slot->price = price;
      }
      levelPtr = slot;
    }

    // Insert all orders in group under level lock (one lock for whole batch)
    {
      std::lock_guard<std::mutex> levelLock(levelPtr->levelMutex);
      for (auto& o : grp) {
        levelPtr->orders.push_back(o);
        levelPtr->orderIters[o->getId()] = std::prev(levelPtr->orders.end());
      }
    }
  }

  // Add all orders to global index under one lock
  {
    std::lock_guard<std::mutex> ordersLock(ordersMutex_);
    for (auto& o : orders) {
      orders_[o->getId()] = o;
    }
  }
}
