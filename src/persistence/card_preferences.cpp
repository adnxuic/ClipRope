#include "persistence/hotkey_settings.hpp"
#include "persistence/optional_preference_table.hpp"
#include <stdexcept>

namespace cliprope::persistence {
application::CardPreferences read_card_preferences(Database& db) {
    std::lock_guard lock(db.mutex());
    Statement row(db.handle(), "SELECT paste_on_double_click,text_font_size,hover_resolution,image_file_previews,"
                               "keep_expanded_group FROM app_settings WHERE id=1");
    if (!row.step()) throw std::runtime_error("Card preferences unavailable");
    application::CardPreferences result{row.integer(0) != 0, row.real(1), row.integer(2) != 0, row.integer(3) != 0, row.integer(4) != 0};
    Statement exists(db.handle(), "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='screenshot_preview_preferences'");
    if(exists.step()) {
        Statement setting(db.handle(), "SELECT enabled FROM screenshot_preview_preferences WHERE id=1");
        if(setting.step()) result.screenshot_hover_preview=setting.integer(0)!=0;
    }
    if (has_preference_table(db.handle(), "clipboard_follow_preferences")) {
        Statement follow(db.handle(), "SELECT card_head,collector FROM clipboard_follow_preferences WHERE id=1");
        if (follow.step()) {
            result.follow_card_head = follow.integer(0) != 0;
            result.follow_collector = follow.integer(1) != 0;
        }
    }
    if (has_preference_table(db.handle(), "card_delete_preferences")) {
        Statement modes(db.handle(), "SELECT recycle_hanging,recycle_floating FROM card_delete_preferences WHERE id=1");
        if (modes.step()) {
            result.recycle_hanging = modes.integer(0) != 0;
            result.recycle_floating = modes.integer(1) != 0;
        }
    }
    return result;
}
void save_card_preferences(Database& db, const application::CardPreferences& value) {
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    Statement set(db.handle(), "UPDATE app_settings SET paste_on_double_click=?,text_font_size=?,hover_resolution=?,"
                               "image_file_previews=?,keep_expanded_group=? WHERE id=1");
    set.bind(1, int(value.paste_on_double_click));
    set.bind(2, value.font_size);
    set.bind(3, int(value.hover_resolution));
    set.bind(4, int(value.image_file_previews));
    set.bind(5, int(value.keep_expanded_group));
    set.run();
    if (sqlite3_changes(db.handle()) != 1) throw std::runtime_error("Card preferences unavailable");
    exec(db.handle(), "CREATE TABLE IF NOT EXISTS screenshot_preview_preferences(id INTEGER PRIMARY KEY CHECK(id=1),"
                      "enabled INTEGER NOT NULL CHECK(enabled IN (0,1))) STRICT");
    Statement preview(db.handle(), "INSERT INTO screenshot_preview_preferences VALUES(1,?) ON CONFLICT(id) DO UPDATE SET enabled=excluded.enabled");
    preview.bind(1,int(value.screenshot_hover_preview)); preview.run();
    exec(db.handle(), "CREATE TABLE IF NOT EXISTS clipboard_follow_preferences(id INTEGER PRIMARY KEY CHECK(id=1),"
                      "card_head INTEGER NOT NULL CHECK(card_head IN (0,1)),"
                      "collector INTEGER NOT NULL CHECK(collector IN (0,1))) STRICT");
    Statement follow(db.handle(), "INSERT INTO clipboard_follow_preferences VALUES(1,?,?) ON CONFLICT(id) "
                                  "DO UPDATE SET card_head=excluded.card_head,collector=excluded.collector");
    follow.bind(1, int(value.follow_card_head));
    follow.bind(2, int(value.follow_collector));
    follow.run();
    exec(db.handle(), "CREATE TABLE IF NOT EXISTS card_delete_preferences(id INTEGER PRIMARY KEY CHECK(id=1),"
                      "recycle_hanging INTEGER NOT NULL CHECK(recycle_hanging IN (0,1)),"
                      "recycle_floating INTEGER NOT NULL CHECK(recycle_floating IN (0,1))) STRICT");
    Statement modes(db.handle(), "INSERT INTO card_delete_preferences(id,recycle_hanging,recycle_floating) VALUES(1,?,?) "
                                 "ON CONFLICT(id) DO UPDATE SET recycle_hanging=excluded.recycle_hanging,"
                                 "recycle_floating=excluded.recycle_floating");
    modes.bind(1, int(value.recycle_hanging));
    modes.bind(2, int(value.recycle_floating));
    modes.run();
    tx.commit();
}
} // namespace cliprope::persistence
