#include "OrderGenerator.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>


/** Construction */ 

OrderGenerator::OrderGenerator(const GeneratorConfig& config)
    : config_(config), rng_(config.seed), nextId_(1) {}

Price OrderGenerator::getMidPrice(const std::string& ticker) const {
    auto it = config_.midPrices.find(ticker);
    if (it != config_.midPrices.end()) return it->second;
    return config_.defaultMidPrice;
}

/** Phase 1 – Initial Book
 *
 * For each ticker, places `initialDepthPerSide` buy orders below mid and
 * the same number of sell orders above mid, one order per price level,
 * with quantities drawn uniformly from [minQ, maxQ].
*/

std::vector<OrderMessage> OrderGenerator::generateInitialBook() {
    std::vector<OrderMessage> messages;
    std::uniform_int_distribution<Quantity> qDist(config_.minQuantity, config_.maxQuantity);

    for (const auto& ticker : config_.tickers) {
        Price mid = getMidPrice(ticker);

        // Buy side: mid - 1*tick, mid - 2*tick, etc
        for (size_t i = 1; i <= config_.initialDepthPerSide; ++i) {
            Price price = mid - static_cast<Price>(i) * config_.tickSize;

            OrderMessage msg;
            msg.action = ActionType::NEW;
            msg.orderType = OrderType::LIMIT;
            msg.orderId = nextId_++;
            msg.ticker = ticker;
            msg.side = Side::BUY;
            msg.price = price;
            msg.quantity = qDist(rng_);

            restingOrders_[ticker].push_back(msg.orderId);
            messages.push_back(msg);
        }

        // Sell side: mid + 1*tick, mid + 2*tick, etc
        for (size_t i = 1; i <= config_.initialDepthPerSide; ++i) {
            Price price = mid + static_cast<Price>(i) * config_.tickSize;

            OrderMessage msg;
            msg.action = ActionType::NEW;
            msg.orderType = OrderType::LIMIT;
            msg.orderId = nextId_++;
            msg.ticker = ticker;
            msg.side = Side::SELL;
            msg.price = price;
            msg.quantity = qDist(rng_);

            restingOrders_[ticker].push_back(msg.orderId);
            messages.push_back(msg);
        }
    }

    return messages;
}

/** Phase 2 – Main Order Stream
 *
 * For each of `numOrders` messages:
 *   1. Pick a ticker uniformly at random.
 *   2. Roll against the limit / market / cancel ratios.
 *   3. Generate the corresponding message.
 * If a cancel is chosen but no orders are resting for that ticker, fall back
 * to a limit order so the total count stays predictable.
*/

std::vector<OrderMessage> OrderGenerator::generateOrders() {
    std::vector<OrderMessage> messages;
    messages.reserve(config_.numOrders);

    std::uniform_int_distribution<size_t> tickerDist(0, config_.tickers.size() - 1);
    std::uniform_real_distribution<double> actionDist(0.0, 1.0);

    double limitCutoff = config_.limitRatio;
    double marketCutoff = limitCutoff + config_.marketRatio;

    for (size_t i = 0; i < config_.numOrders; ++i) {
        const std::string& ticker = config_.tickers[tickerDist(rng_)];
        double roll = actionDist(rng_);

        if (roll < limitCutoff) {
            messages.push_back(generateLimitOrder(ticker));
        } else if (roll < marketCutoff) {
            messages.push_back(generateMarketOrder(ticker));
        } else {
            if (!restingOrders_[ticker].empty()) {
                messages.push_back(generateCancelOrder(ticker));
            } else {
                messages.push_back(generateLimitOrder(ticker));
            }
        }
    }

    return messages;
}

std::vector<OrderMessage> OrderGenerator::generateAll() {
    auto initial = generateInitialBook();
    auto stream = generateOrders();
    initial.insert(initial.end(),
                   std::make_move_iterator(stream.begin()),
                   std::make_move_iterator(stream.end()));
    return initial;
}

/** Individual message generators */ 
OrderMessage OrderGenerator::generateLimitOrder(const std::string& ticker) {
    Price mid = getMidPrice(ticker);

    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> offsetDist(0, config_.maxPriceOffsetTicks);
    std::uniform_int_distribution<Quantity> qDist(config_.minQuantity, config_.maxQuantity);

    OrderMessage msg;
    msg.action = ActionType::NEW;
    msg.orderType = OrderType::LIMIT;
    msg.orderId = nextId_++;
    msg.ticker = ticker;
    msg.side = sideDist(rng_) == 0 ? Side::BUY : Side::SELL;
    msg.quantity = qDist(rng_);

    int offset = offsetDist(rng_);
    if (msg.side == Side::BUY) {
        // Buys at or below mid
        msg.price = mid - static_cast<Price>(offset) * config_.tickSize;
    } else {
        // Sells at or above mid
        msg.price = mid + static_cast<Price>(offset) * config_.tickSize;
    }

    restingOrders_[ticker].push_back(msg.orderId);
    return msg;
}

OrderMessage OrderGenerator::generateMarketOrder(const std::string& ticker) {
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<Quantity> qDist(config_.minQuantity, config_.maxQuantity);

    OrderMessage msg;
    msg.action = ActionType::NEW;
    msg.orderType = OrderType::MARKET;
    msg.orderId = nextId_++;
    msg.ticker = ticker;
    msg.side = sideDist(rng_) == 0 ? Side::BUY : Side::SELL;
    msg.price = 0;  // matching engine fills at best available
    msg.quantity = qDist(rng_);

    // Market orders execute immediately; they do not rest in the book.
    return msg;
}

OrderMessage OrderGenerator::generateCancelOrder(const std::string& ticker) {
    auto& resting = restingOrders_[ticker];
    std::uniform_int_distribution<size_t> idxDist(0, resting.size() - 1);
    size_t idx = idxDist(rng_);

    OrderMessage msg;
    msg.action = ActionType::CANCEL;
    msg.orderType = OrderType::LIMIT;
    msg.orderId = resting[idx];
    msg.ticker = ticker;
    msg.side = Side::BUY;  // side is unused for cancels
    msg.price = 0;
    msg.quantity  = 0;

    // O(1) removal: swap with back and pop
    resting[idx] = resting.back();
    resting.pop_back();

    return msg;
}

/** CSV serialization
 Format:  action,type,id,ticker,side,price,quantity
*/

static const char* actionStr(ActionType a) {
    switch (a) {
        case ActionType::NEW:    return "NEW";
        case ActionType::CANCEL: return "CANCEL";
    }
    return "UNKNOWN";
}

static const char* typeStr(OrderType t) {
    switch (t) {
        case OrderType::LIMIT:  return "LIMIT";
        case OrderType::MARKET: return "MARKET";
    }
    return "UNKNOWN";
}

static const char* sideStr(Side s) {
    switch (s) {
        case Side::BUY:  return "BUY";
        case Side::SELL: return "SELL";
    }
    return "UNKNOWN";
}

void OrderGenerator::writeToCSV(const std::string& filepath,
                                const std::vector<OrderMessage>& messages) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        throw std::runtime_error("OrderGenerator::writeToCSV: cannot open " + filepath);
    }

    out << "action,type,id,ticker,side,price,quantity\n";
    for (const auto& m : messages) {
        out << actionStr(m.action) << ','
            << typeStr(m.orderType) << ','
            << m.orderId << ','
            << m.ticker << ','
            << sideStr(m.side) << ','
            << m.price << ','
            << m.quantity << '\n';
    }
}
