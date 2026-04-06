#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Order.h"
#include "Types.h"

/** Single execution event produced by the matcher.
 *
 * `ticker` is left empty by the book itself (which is single-symbol and has
 * no notion of one) and stamped by the MatchingEngine after the book call.
 */
struct Trade {
  Id buyOrderId;
  Id sellOrderId;
  Price price;
  Quantity quantity;
  std::string ticker;
};

/** Read-only snapshot of an entire book at a point in time.
 *
 * Built in O(N) over all resting orders. Intended for end-of-run inspection
 * and golden-trace verification, not for hot paths.
 *
 * Within each side, levels are listed in ascending price order; within each
 * level, orders are listed in FIFO (time-priority) order. This makes the
 * dump byte-stable across implementations as long as they preserve the
 * standard price-time priority invariant.
 */
struct BookSnapshot {
  struct OrderView {
    Id id;
    Quantity remaining;
  };
  struct LevelView {
    Price price;
    std::vector<OrderView> orders;
  };
  std::vector<LevelView> bids;
  std::vector<LevelView> asks;
};

/** Single-symbol limit order book.
 *
 * Internal layout (price-time priority):
 *   - bids_ / asks_  : ordered map<Price, PriceLevel>. For bids the best
 *      level is the largest key (`prev(end())`); for asks it
 *      is the smallest (`begin()`). std::map gives O(log M)
 *       level insert/erase and O(1) best access.
 *   - PriceLevel : FIFO of resting OrderPointers + an id->iterator map
 *         so cancels are O(1) inside the level.
 *   - orders_ : global id -> OrderPointer for O(1) lookup on cancel / modify.
 *
 * The book is single-threaded; thread safety belongs to the command-queue
 * layer that will sit on top of this class.
 */
class LimitOrderBook {
public:
  // Match the incoming order against resting liquidity at crossing prices,
  // then rest any unfilled remainder. Returns trades in execution order.
  std::vector<Trade> addLimitOrder(Id id, Side side, Price price, Quantity quantity);

  // Walk the opposite side at any price until filled or empty. Unfilled
  // remainder is dropped (market orders never rest).
  std::vector<Trade> addMarketOrder(Id id, Side side, Quantity quantity);

  // Remove a resting order. No-op if the id is unknown (e.g. already filled).
  void cancelOrder(Id id);

  // Cancel + re-add: the modified order goes to the back of its (possibly
  // new) price level, losing time priority — standard exchange behaviour.
  std::vector<Trade> modifyOrder(Id id, Price newPrice, Quantity newQuantity);

  bool hasOrder(Id id) const;
  std::size_t restingOrderCount() const;
  std::size_t bidLevelCount() const;
  std::size_t askLevelCount() const;

  // Build a deep, deterministic view of every resting order. O(N).
  BookSnapshot snapshot() const;

private:
  struct PriceLevel {
    Price price;
    std::list<OrderPointer> orders;
    std::unordered_map<Id, std::list<OrderPointer>::iterator> orderIters;
  };
  using PriceLevelPointer = std::shared_ptr<PriceLevel>;

  // Match 'incoming' (a buy) against asks_ at price <= maxPrice, lowest first.
  void matchBuyAgainstAsks(const OrderPointer& incoming, Price maxPrice,
        std::vector<Trade>& trades);

  // Match 'incoming' (a sell) against bids_ at price >= minPrice, highest first.
  void matchSellAgainstBids(const OrderPointer& incoming, Price minPrice,
          std::vector<Trade>& trades);

  // Append 'order' to the FIFO at its price level, creating the level if
  // needed, and register it in the global id index.
  void rest(const OrderPointer& order);

  std::map<Price, PriceLevelPointer> bids_;
  std::map<Price, PriceLevelPointer> asks_;
  std::unordered_map<Id, OrderPointer> orders_;
};
