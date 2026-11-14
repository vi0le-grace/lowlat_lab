
#pragma once

#include <cstdint>

namespace lob {

using Price = std::int32_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;
using Seq = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell
};

enum class Status : std::uint8_t {
    Accepted,
    RejectedDuplicate,
    RejectedQty
};

struct Order {
    OrderId id;
    Price   price;
    Qty     qty;
    Side    side;
};

struct Trade {
    Seq     seq;
    OrderId takerId;
    OrderId makerId;
    Price   price;
    Qty     qty;
    Side    takerSide;
};

inline bool operator==(const Trade& a, const Trade& b) {
    return a.seq == b.seq && a.takerId == b.takerId && a.makerId == b.makerId &&
        a.price == b.price && a.qty == b.qty && a.takerSide == b.takerSide;
}
inline bool operator!=(const Trade& a, const Trade& b) { return !(a == b); }

struct Stats {
    std::uint64_t accepted = 0;
    std::uint64_t trades = 0;
    std::uint64_t matchedQty = 0;
    std::uint64_t rejectedQty = 0;
    std::uint64_t rejectedDuplicate = 0;
    std::uint64_t cancelUnknown = 0;
};

// One visible price level, used for differential comparison and printing.
struct LevelView {
    Side side;
    Price price;
    std::uint64_t qty;

    bool operator==(const LevelView& o) const {
        return side == o.side && price == o.price && qty == o.qty;
    }
    bool operator!=(const LevelView& o) const { return !(*this == o); }
};

inline const char* status_name(Status s) {
    switch (s) {
        case Status::Accepted:          return "Accepted";
        case Status::RejectedDuplicate: return "RejectedDuplicate";
        case Status::RejectedQty:       return "RejectedQty";
    }
    return "?";
}
}   // namespace lob