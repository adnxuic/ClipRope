#include "persistence/localization_settings.hpp"

namespace cliprope::persistence {
localization::Language read_language(Database& db) {
    std::lock_guard lock(db.mutex());
    Statement exists(db.handle(), "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='localization_preferences'");
    if (!exists.step()) return localization::Language::automatic;
    Statement row(db.handle(), "SELECT language FROM localization_preferences WHERE id=1");
    return row.step() ? localization::parse_language(row.text(0)) : localization::Language::automatic;
}
void save_language(Database& db, localization::Language language) {
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    exec(db.handle(), "CREATE TABLE IF NOT EXISTS localization_preferences("
        "id INTEGER PRIMARY KEY CHECK(id=1),language TEXT NOT NULL "
        "CHECK(language IN ('auto','en-US','zh-CN'))) STRICT");
    Statement write(db.handle(), "INSERT INTO localization_preferences VALUES(1,?) "
        "ON CONFLICT(id) DO UPDATE SET language=excluded.language");
    write.bind(1, localization::language_code(language));
    write.run();
    db.fault("language-before-commit");
    tx.commit();
}
} // namespace cliprope::persistence
