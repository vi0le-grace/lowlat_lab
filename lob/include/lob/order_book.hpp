// order_book.hpp
//
// Single-threaded limit order book, price-time priority. Limit orders only.
//
//  - A buy matches resting sells at or below its price, lowest price first and
//    oldest order first within a price; a sell mirrors this.
//  - Trades execute at the resting (maker) order's price.
//  - A partial fill leaves the remainder resting at the order's limit price.
//  - qty == 0 is rejected. An id that is currently resting is rejected as a
//    duplicate (ids of fully filled or cancelled orders may be reused).
//  - cancel(id) returns false if the id is not resting.
//  - Not supported: market orders, IOC/FOK, modify, self-trade prevention.
//
// Trades are reported through a sink with onTrade(const Trade&). Resting an
// order allocates (list and index nodes); matching does not allocate a result.

#pragma once

#include <algorithm>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class OrderBook {
private:
    struct Level {
        std::list<Order> orders;
        std::uint64_t totalQty = 0;
    };

    struct Location {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    // Consume as much of `level` as possible; returns the unfilled remainder.
    template<class Sink>
    Qty match_level(const Order& taker, Price price, Qty remaining, Level& level, Sink& sink) {
        while (remaining > 0 && !level.orders.empty()) {
            Order& maker = level.orders.front();
            const Qty fill = std::min(remaining, maker.qty);

            sink.onTrade(Trade{++tradeSeq_, taker.id, maker.id, price, fill, taker.side});
            ++stats_.trades;
            stats_.matchedQty += fill;

            remaining -= fill;
            maker.qty -= fill;
            level.totalQty -= fill;

            if (maker.qty == 0) {
                index_.erase(maker.id);
                level.orders.pop_front();
            }
        }
        return remaining;
    }

    void rest_(const Order& o, Qty qty) {
        const Order node{o.id, o.price, qty, o.side};
        if (o.side == Side::Buy) {
            Level& level = bids_[o.price];
            level.orders.push_back(node);
            level.totalQty += qty;
            index_.emplace(o.id, Location{Side::Buy, o.price, std::prev(level.orders.end())});
        } else {
            Level& level = asks_[o.price];
            level.orders.push_back(node);
            level.totalQty += qty;
            index_.emplace(o.id, Location{Side::Sell, o.price, std::prev(level.orders.end())});
        }
    }

    std::map<Price, Level, std::greater<Price>> bids_;  // highest price first
    std::map<Price, Level, std::less<Price>> asks_;  // lowest price first
    std::unordered_map<OrderId, Location> index_;
    Seq tradeSeq_ = 0;
    Stats stats_;

public:
    template<class Sink>
    Status add(const Order& o, Sink&& sink) {
        if (o.qty == 0) {
            ++stats_.rejectedQty;
            return Status::RejectedQty;
        }
        if (index_.find(o.id) != index_.end()) {
            ++stats_.rejectedDuplicate;
            return Status::RejectedDuplicate;
        }

        Qty remaining = o.qty;

        if (o.side == Side::Buy) {
            while (remaining > 0 && !asks_.empty()) {
                auto it = asks_.begin();
                if (it->first > o.price) break;   // best ask no longer crosses
                remaining = match_level(o, it->first, remaining, it->second, sink);
                if (it->second.orders.empty()) asks_.erase(it);
            }
        } else {
            while (remaining > 0 && !bids_.empty()) {
                auto it = bids_.begin();
                if (it->first < o.price) break;   // best bid no longer crosses
                remaining = match_level(o, it->first, remaining, it->second, sink);
                if (it->second.orders.empty()) bids_.erase(it);
            }
        }

        if (remaining > 0) rest_(o, remaining);
        ++stats_.accepted;
        return Status::Accepted;
    }

    bool cancel(OrderId id) {
        auto found = index_.find(id);
        if (found == index_.end()) {
            ++stats_.cancelUnknown;
            return false;
        }

        const Location loc = found->second;
        if (loc.side == Side::Buy) {
            auto levelIt = bids_.find(loc.price);
            levelIt->second.totalQty -= loc.it->qty;
            levelIt->second.orders.erase(loc.it);
            if (levelIt->second.orders.empty()) bids_.erase(levelIt);
        } else {
            auto levelIt = asks_.find(loc.price);
            levelIt->second.totalQty -= loc.it->qty;
            levelIt->second.orders.erase(loc.it);
            if (levelIt->second.orders.empty()) asks_.erase(levelIt);
        }

        index_.erase(found);
        return true;
    }

    // Top-N levels per side: bids best-first, then asks best-first.
    // Test/print only: allocates, so keep it out of the benchmark hot path.
    std::vector<LevelView> depth(int n) const {
        std::vector<LevelView> out;
        if (n <= 0) return out;
        out.reserve(static_cast<std::size_t>(n) * 2);

        int shown = 0;
        for (const auto& entry : bids_) {
            if (shown++ >= n) break;
            out.push_back({Side::Buy, entry.first, entry.second.totalQty});
        }
        shown = 0;
        for (const auto& entry : asks_) {
            if (shown++ >= n) break;
            out.push_back({Side::Sell, entry.first, entry.second.totalQty});
        }
        return out;
    }

    const Stats& stats() const { return stats_; }
};

}   // namespace lob
