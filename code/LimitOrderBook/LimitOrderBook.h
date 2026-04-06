#pragma once

#include <vector>

#include "Order.h"
#include "Types.h"

/** Skeleton order book.
 *
 * Currently exposes only a NEW-LIMIT insertion path: the caller hands in the
 * fields from a NEW LIMIT order message, the book constructs an `Order` and
 * stores it. Matching, cancels, and price-time priority will live here later.
 */
class LimitOrderBook {
public:
  void addLimitOrder(Id id, Side side, Price price, Quantity quantity);

private:
  std::vector<Order> orders_;
};
