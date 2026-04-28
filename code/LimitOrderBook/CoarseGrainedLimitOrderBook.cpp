#include "CoarseGrainedLimitOrderBook.h"

std::vector<Trade> CoarseGrainedLimitOrderBook::addLimitOrder(
    Id id, Side side, Price price, Quantity quantity) {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.addLimitOrder(id, side, price, quantity);
}

std::vector<Trade> CoarseGrainedLimitOrderBook::addMarketOrder(
    Id id, Side side, Quantity quantity) {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.addMarketOrder(id, side, quantity);
}

void CoarseGrainedLimitOrderBook::cancelOrder(Id id) {
  std::lock_guard<std::mutex> lock(mutex_);
  book_.cancelOrder(id);
}

std::vector<Trade> CoarseGrainedLimitOrderBook::modifyOrder(
    Id id, Price newPrice, Quantity newQuantity) {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.modifyOrder(id, newPrice, newQuantity);
}

bool CoarseGrainedLimitOrderBook::hasOrder(Id id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.hasOrder(id);
}

std::size_t CoarseGrainedLimitOrderBook::restingOrderCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.restingOrderCount();
}

std::size_t CoarseGrainedLimitOrderBook::bidLevelCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.bidLevelCount();
}

std::size_t CoarseGrainedLimitOrderBook::askLevelCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.askLevelCount();
}

BookSnapshot CoarseGrainedLimitOrderBook::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.snapshot();
}

bool CoarseGrainedLimitOrderBook::wouldCross(Side side, Price price) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return book_.wouldCross(side, price);
}

void CoarseGrainedLimitOrderBook::batchRest(std::vector<OrderPointer> orders) {
  std::lock_guard<std::mutex> lock(mutex_);
  book_.batchRest(std::move(orders));
}
