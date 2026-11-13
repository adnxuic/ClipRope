#include "persistence/hotkey_settings.hpp"
#include "persistence/optional_preference_table.hpp"
#include <stdexcept>

namespace cliprope::persistence {
namespace {
void ensure_general(sqlite3* db) {
    exec(db, "CREATE TABLE IF NOT EXISTS general_preferences(id INTEGER PRIMARY KEY CHECK(id=1),"
             "topmost INTEGER NOT NULL DEFAULT 1 CHECK(topmost IN (0,1)),"
             "undo_sign_in INTEGER NOT NULL DEFAULT 0 CHECK(undo_sign_in IN (0,1))) STRICT");
    exec(db, "INSERT OR IGNORE INTO general_preferences(id) VALUES(1)");
}
}
bool read_workspace_topmost(Database& db) {
    std::lock_guard lock(db.mutex());
    if (!has_preference_table(db.handle(), "general_preferences")) return true;
    Statement row(db.handle(), "SELECT topmost FROM general_preferences WHERE id=1");
    return !row.step() || row.integer(0) != 0;
}
void save_workspace_topmost(Database& db, bool topmost) {
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    ensure_general(db.handle());
    Statement set(db.handle(), "UPDATE general_preferences SET topmost=? WHERE id=1");
    set.bind(1, int(topmost));
    set.run();
    tx.commit();
}
bool pending_sign_in_rollback(Database& db) {
    std::lock_guard lock(db.mutex());
    if (!has_preference_table(db.handle(), "general_preferences")) return false;
    Statement row(db.handle(), "SELECT undo_sign_in FROM general_preferences WHERE id=1");
    return row.step() && row.integer(0) != 0;
}
void mark_sign_in_rollback(Database& db, bool pending) {
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    ensure_general(db.handle());
    Statement set(db.handle(), "UPDATE general_preferences SET undo_sign_in=? WHERE id=1");
    set.bind(1, int(pending));
    set.run();
    tx.commit();
}
void reset_global_preferences(Database& db, bool finish_sign_in) {
    std::lock_guard lock(db.mutex());
    auto handle = db.handle();
    Transaction tx(handle);
    ensure_general(handle);
    exec(handle, "UPDATE general_preferences SET topmost=1 WHERE id=1");
    if (finish_sign_in) exec(handle, "UPDATE general_preferences SET undo_sign_in=0 WHERE id=1");
    exec(handle, "UPDATE app_settings SET paste_on_double_click=1,text_font_size=14,hover_resolution=1,"
                 "image_file_previews=1,keep_expanded_group=1,default_capacity=100 WHERE id=1");
    if (sqlite3_changes(handle) != 1) throw std::runtime_error("Global settings unavailable.");
    if (has_preference_table(handle, "appearance_preferences")) exec(handle, "DELETE FROM appearance_preferences WHERE id=1");
    if (has_preference_table(handle, "shortcut_preferences")) exec(handle, "DELETE FROM shortcut_preferences WHERE action BETWEEN 0 AND 4");
    if (has_preference_table(handle, "localization_preferences")) exec(handle, "DELETE FROM localization_preferences WHERE id=1");
    if (has_preference_table(handle, "screenshot_preview_preferences")) exec(handle, "DELETE FROM screenshot_preview_preferences WHERE id=1");
    if (has_preference_table(handle, "clipboard_follow_preferences")) exec(handle, "DELETE FROM clipboard_follow_preferences WHERE id=1");
    if (has_preference_table(handle, "card_delete_preferences")) exec(handle, "DELETE FROM card_delete_preferences WHERE id=1");
    exec(handle, "DELETE FROM recent_colors");
    tx.commit();
}
} // namespace cliprope::persistence
