#include "MatchingEngine.h"

#include <stdexcept>

std::vector<Trade> MatchingEngine::onMessage(const OrderMessage& msg) {
  LimitOrderBook& book = bookForMut(msg.ticker);

  std::vector<Trade> trades;
  switch (msg.action) {
    case ActionType::NEW:
      switch (msg.orderType) {
        case OrderType::LIMIT:
          trades = book.addLimitOrder(msg.orderId, msg.side, msg.price, msg.quantity);
          break;
        case OrderType::MARKET:
          trades = book.addMarketOrder(msg.orderId, msg.side, msg.quantity);
          break;
      }
      break;

    case ActionType::CANCEL:
      // Generator tracks resting ids best-effort, so the target may already
      // have been filled by an earlier message in this batch — then
      // cancelOrder is a no-op
      book.cancelOrder(msg.orderId);
      return {};
  }

  // Stamp the symbol on every trade — the book is single-symbol and leaves
  // the field blank.
  for (auto& t : trades) {
    t.ticker = msg.ticker;
  }
  return trades;
}

std::vector<Trade> MatchingEngine::processAll(const std::vector<OrderMessage>& msgs) {
  std::vector<Trade> all;
  all.reserve(msgs.size());
  for (const auto& msg : msgs) {
    auto trades = onMessage(msg);
    all.insert(all.end(),
               std::make_move_iterator(trades.begin()),
               std::make_move_iterator(trades.end()));
  }
  return all;
}

const LimitOrderBook* MatchingEngine::bookFor(const std::string& ticker) const {
  auto it = books_.find(ticker);
  return it == books_.end() ? nullptr : it->second.get();
}

LimitOrderBook& MatchingEngine::bookForMut(const std::string& ticker) {
  auto it = books_.find(ticker);
  if (it == books_.end()) {
    it = books_.emplace(ticker, std::make_unique<LimitOrderBook>()).first;
  }
  return *it->second;
}
