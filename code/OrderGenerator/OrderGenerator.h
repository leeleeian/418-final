#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "../LimitOrderBook/Types.h"


/** Message types used by the generator -- 
 * matching engine later determines how to process them based on action + order type. */ 
enum class OrderType { LIMIT, MARKET };
enum class ActionType { NEW, CANCEL };

struct OrderMessage {
    ActionType action;
    OrderType orderType;
    Id orderId;
    std::string ticker;
    Side side;
    Price price; // 0 for market orders / cancels
    Quantity quantity; // 0 for cancels
};

/** Configuration for the Controlled Baseline Generator (Tier 1).
 * All prices are in integer ticks (e.g. $100.00 = 10000 if tick = 1 cent).
 */
struct GeneratorConfig {
    uint64_t seed = 42;

    // Tickers to generate orders for
    std::vector<std::string> tickers = {"AAPL"};

    // Reference mid-price used for every ticker (per-ticker overrides below)
    Price defaultMidPrice = 10000;

    // Per-ticker mid-price overrides (looked up first; falls back to default)
    std::unordered_map<std::string, Price> midPrices;

    // Minimum price increment
    Price tickSize = 1;

    // Total number of orders in the main stream (excludes initial book)
    size_t numOrders = 10000;

    // Action type -- mix ratios (sums to 1.0)
    double limitRatio = 0.60;
    double marketRatio = 0.20;
    double cancelRatio = 0.20;

    // Max offset from mid-price in number of ticks for limit orders
    int maxPriceOffsetTicks = 20;

    // Quantity drawn uniformly from [minQuantity, maxQuantity]
    Quantity minQuantity = 1;
    Quantity maxQuantity = 100;

    // Number of resting orders seeded per side per ticker
    size_t initialDepthPerSide = 10;
};

/** Controlled Baseline Generator

 Produces an i.i.d. order stream suitable for correctness verification and
 coarse-grained-locking benchmarks.  Two phases:
   1. generateInitialBook() – symmetric depth around each ticker's mid
   2. generateOrders() – random mix of limit / market / cancel 
*/
class OrderGenerator {
public:
    explicit OrderGenerator(const GeneratorConfig& config);

    // Phase 1: seed orders forming the initial book
    std::vector<OrderMessage> generateInitialBook();

    // Phase 2: main order stream
    std::vector<OrderMessage> generateOrders();

    // Convenience: initial book + order stream concatenated
    std::vector<OrderMessage> generateAll();

    // Serialize messages to CSV for offline inspection / scripting
    static void writeToCSV(const std::string& filepath,
                           const std::vector<OrderMessage>& messages);

private:
    GeneratorConfig config_;
    std::mt19937_64 rng_;
    Id nextId_;

    // IDs of limit orders that *may* still be resting (best-effort tracking;
    // the generator does not model fills, so some cancel targets may already
    // have been filled by the time the matching engine processes them).
    std::unordered_map<std::string, std::vector<Id>> restingOrders_;

    Price getMidPrice(const std::string& ticker) const;
    OrderMessage generateLimitOrder(const std::string& ticker);
    OrderMessage generateMarketOrder(const std::string& ticker);
    OrderMessage generateCancelOrder(const std::string& ticker);
};
