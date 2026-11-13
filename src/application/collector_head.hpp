#pragma once
#include "application/card_preferences.hpp"
#include "domain/history.hpp"

namespace cliprope::application {
struct CollectorHead {
    domain::Id rope, entry, snapshot;
    bool operator==(const CollectorHead&) const = default;
};

struct HeadChange {
    CollectorHead before, after;
    std::uint64_t serial{}; // Writer order across UI commands and background text saves.

    bool follows(const CardPreferences& preferences) const {
        return !after.entry.empty() && before != after &&
               (before.rope == after.rope ? preferences.follow_card_head : preferences.follow_collector);
    }
};
}
