#include "persistence/hotkey_settings.hpp"
#include "persistence/color_store.hpp"
#include <stdexcept>

namespace cliprope::persistence {
application::Appearance read_appearance(Database& db) {
    std::lock_guard lock(db.mutex());
    Statement exists(db.handle(), "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='appearance_preferences'");
    if (!exists.step()) return {};
    Statement row(db.handle(), "SELECT theme,match_value FROM appearance_preferences WHERE id=1");
    if (!row.step()) return {};
    return {static_cast<application::Theme>(row.integer(0)), domain::ColorSelection::decode(row.text(1))};
}
void save_appearance(Database& db, application::Appearance value) {
    if (value.match_color.custom && value.match_color.custom->a != 255)
        throw std::invalid_argument("Find highlight must be opaque");
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    exec(db.handle(), "CREATE TABLE IF NOT EXISTS appearance_preferences(id INTEGER PRIMARY KEY CHECK(id=1),"
                      "theme INTEGER NOT NULL CHECK(theme BETWEEN 0 AND 2),"
                      "match_value TEXT NOT NULL DEFAULT 'default') STRICT");
    Statement write(db.handle(), "INSERT OR REPLACE INTO appearance_preferences VALUES(1,?,?)");
    write.bind(1, static_cast<int>(value.theme));
    write.bind(2, value.match_color.encode());
    write.run();
    tx.commit();
}
void save_match_color(Database& db, domain::ColorSelection original, domain::ColorSelection selected) {
    if (selected.custom && selected.custom->a != 255) throw std::invalid_argument("Find highlight must be opaque");
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    const auto current = read_appearance(db);
    if (current.match_color != original) throw std::runtime_error("Color changed");
    Statement write(db.handle(), "INSERT INTO appearance_preferences(id,theme,match_value) VALUES(1,?,?) "
                                 "ON CONFLICT(id) DO UPDATE SET match_value=excluded.match_value");
    write.bind(1, int(current.theme));
    write.bind(2, selected.encode());
    write.run();
    if (selected != original) remember_color(db.handle(), selected);
    tx.commit();
}
} // namespace cliprope::persistence
