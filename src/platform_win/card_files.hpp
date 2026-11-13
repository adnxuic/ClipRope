#pragma once
#include "platform_win/card_editor.hpp"
#include "persistence/history_store.hpp"

namespace cliprope::win {
std::filesystem::path find_snip_original(persistence::HistoryStore&, const domain::Entry&, const domain::Snapshot&);
std::filesystem::path bind_snip_original(persistence::HistoryStore&, const domain::Entry&, const domain::Snapshot&);
struct ScreenshotTarget {
    std::filesystem::path path;
    bool folder_only{};
};
ScreenshotTarget screenshot_target(persistence::HistoryStore&, const domain::Entry&, const domain::Snapshot&, CardCommand,
                                   std::stop_token stop = {});
void visit_card_files(HWND owner, CardCommand command, const application::CardSummary& card,
                      persistence::HistoryStore& store, const ScreenshotTarget* prepared = nullptr);
}
