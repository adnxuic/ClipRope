#pragma once
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"

namespace cliprope::host {
inline void confirm_menu_observation(persistence::HistoryStore& store, rendering::CardBrowser& browser,
                                     application::CardSummary& card, rendering::CardBrowser::PathLookup lookup) {
    browser.inspect_path_kind(card, lookup);
    if (!card.unclassified_path || card.observed_path_kind == domain::PathKind::unknown) return;
    store.save_path_kind(card.entry, card.observed_path_kind);
    card = browser.summary(card.entry.id, true);
}
}
