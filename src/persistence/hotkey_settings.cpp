#include "persistence/hotkey_settings.hpp"
#include <stdexcept>

namespace cliprope::persistence {
application::ShortcutBindings read_shortcuts(Database& db) {
    std::lock_guard lock(db.mutex());
    auto bindings = application::default_shortcuts();
    Statement exists(db.handle(), "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='shortcut_preferences'");
    if (!exists.step()) return bindings;
    Statement rows(db.handle(), "SELECT action,enabled,chord FROM shortcut_preferences");
    while (rows.step()) {
        const auto id = rows.integer(0);
        if (id >= 0 && id < int(bindings.size())) bindings[id] = {rows.integer(1) != 0, rows.text(2)};
    }
    return bindings;
}
void save_shortcut(Database& db, application::ShortcutAction action, const application::ShortcutBinding& binding) {
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    exec(db.handle(), "CREATE TABLE IF NOT EXISTS shortcut_preferences(action INTEGER PRIMARY KEY,"
                      "enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),chord TEXT NOT NULL) STRICT");
    Statement row(db.handle(), "INSERT OR REPLACE INTO shortcut_preferences VALUES(?,?,?)");
    row.bind(1, int(action));
    row.bind(2, int(binding.enabled));
    row.bind(3, binding.chord);
    row.run();
    tx.commit();
}
} // namespace cliprope::persistence
