#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/browse.hpp"
#include <iostream>

int main() {
    using namespace cliprope;
    using history_fixture::require;
    try {
        history_fixture::Directory profile;
        domain::Id id;
        {
            persistence::HistoryStore store(profile.path);
            id = store.admit("main", {{{13, {}, {'A', 0, 0, 0}}}});
            persistence::Browser browser(store);
            const auto changes = sqlite3_total_changes64(store.database().handle());
            persistence::PageReadStats stats;
            const auto page = browser.page("main", 0, 1, persistence::FileListExtent::prefix, &stats);
            require(page.size() == 1 && page.front().entry.id == id, "History page lost the capture");
            require(stats.preview_requests == 0 && sqlite3_total_changes64(store.database().handle()) == changes,
                    "History read wrote data or requested pixels");
            store.detach(id);
            require(!store.entry(id).rope_id, "Detach retained rope membership");
            store.rehang(id, "main");
        }
        persistence::HistoryStore reopened(profile.path);
        require(reopened.entry(id).rope_id == "main" && reopened.floating().empty(), "Return did not survive reopen");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
