#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/localization_settings.hpp"
#include <iostream>

using namespace cliprope;
using namespace history_fixture;
using namespace persistence;
using localization::Language;
int main() {
    try {
        Directory temp;
        bool fail{};
        const auto root = temp.path / "profile", backup = temp.path / "backup";
        domain::Id id;
        {
            HistoryStore store(root, [&](std::string_view point) {
                if (fail && point == "language-before-commit") throw std::runtime_error("Injected language write failure");
            });
            auto& db = store.database();
            require(read_language(db) == Language::automatic, "Old profile did not default to Auto");
            domain::Format text;
            text.standard_id = 13;
            text.bytes = {'O', 0, 'n', 0, 0, 0};
            id = store.admit("main", {{text}});
            const auto original = store.entry(id);
            save_language(db, Language::english);
            save_language(db, Language::simplified_chinese);
            fail = true;
            rejects([&] { save_language(db, Language::english); }, "Injected write succeeded");
            fail = false;
            require(read_language(db) == Language::simplified_chinese, "Failed write changed language");
            save_workspace_topmost(db, false);
            exec(db.handle(), "CREATE TRIGGER fail_language_reset BEFORE DELETE ON localization_preferences "
                "BEGIN SELECT RAISE(ABORT,'Injected reset failure'); END");
            rejects([&] { reset_global_preferences(db); }, "Injected reset succeeded");
            require(read_language(db) == Language::simplified_chinese && !read_workspace_topmost(db),
                    "Failed reset did not roll back all preferences");
            exec(db.handle(), "DROP TRIGGER fail_language_reset");
            reset_global_preferences(db);
            require(read_language(db) == Language::automatic && read_workspace_topmost(db), "Reset did not restore Auto");
            save_language(db, Language::english);
            exec(db.handle(), "PRAGMA ignore_check_constraints=ON; UPDATE localization_preferences SET language='future';");
            require(read_language(db) == Language::automatic, "Unknown language was not tolerated");
            exec(db.handle(), "UPDATE localization_preferences SET language='en-US'; PRAGMA ignore_check_constraints=OFF;");
            save_language(db, Language::simplified_chinese);
            const auto current = store.entry(id);
            require(current.snapshot_id == original.snapshot_id && current.revision == original.revision &&
                    current.rope_id == original.rope_id && current.sort_key == original.sort_key,
                    "Language changed history identity, content or order");
            Statement schema(db.handle(), "PRAGMA user_version");
            require(schema.step() && schema.integer(0) == 11, "Language preference changed the core schema");
            db.backup_to(backup);
        }
        {
            HistoryStore reopened(root);
            require(read_language(reopened.database()) == Language::simplified_chinese, "Language did not survive restart");
        }
        Database::restore_backup(backup, temp.path / "restored");
        {
            HistoryStore restored(temp.path / "restored");
            require(read_language(restored.database()) == Language::simplified_chinese && restored.entry(id).rope_id == "main",
                    "Backup lost language or history");
        }
        std::cout << "Language persistence, rollback, reset, backup and history isolation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
