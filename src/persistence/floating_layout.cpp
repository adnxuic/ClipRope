#include "persistence/history_store.hpp"
#include <cmath>
#include <stdexcept>

namespace cliprope::persistence {

void HistoryStore::write_floating_layout(std::string_view id, const domain::Layout& layout) {
    if (!std::isfinite(layout.x) || !std::isfinite(layout.y) ||
        !std::isfinite(layout.width) || !std::isfinite(layout.height) ||
        layout.width <= 0 || layout.height <= 0 ||
        layout.monitor.size() > 1024 || layout.monitor.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid Floating card layout");
    auto* db = db_.handle();
    Statement placement(db, "UPDATE card_presentation SET monitor=?,x=?,y=?,width=?,height=?,visible=? "
                            "WHERE entry_id=? AND EXISTS(SELECT 1 FROM entries e WHERE e.id=entry_id AND e.state='Floating')");
    placement.bind(1, layout.monitor);
    placement.bind(2, layout.x);
    placement.bind(3, layout.y);
    placement.bind(4, layout.width);
    placement.bind(5, layout.height);
    placement.bind(6, static_cast<int>(layout.visible));
    placement.bind(7, id);
    placement.run();
    if (sqlite3_changes(db) != 1) throw std::runtime_error("Floating card no longer exists");
}

void HistoryStore::save_layout(std::string_view id, const domain::Layout& layout) {
    mutate([&] { write_floating_layout(id, layout); });
}
} // namespace cliprope::persistence
