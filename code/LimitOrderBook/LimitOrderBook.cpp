#include "LimitOrderBook.h"

void LimitOrderBook::addLimitOrder(Id id, Side side, Price price, Quantity quantity) {
  orders_.emplace_back(id, side, price, quantity);
}
