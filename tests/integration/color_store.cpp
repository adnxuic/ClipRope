#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/browse.hpp"
#include "persistence/color_store.hpp"
#include "persistence/hotkey_settings.hpp"
#include <windows.h>
#include <iostream>

using namespace cliprope;
using namespace persistence;
using history_fixture::require;
void match_color() {
    history_fixture::Directory profile;
    const domain::ColorSelection blue(0x124578), gold(0x987611);
    {
        HistoryStore store(profile.path);
        auto& db = store.database();
        save_appearance(db, {application::Theme::dark, blue});
        history_fixture::rejects([&] { save_match_color(db, {}, gold); }, "Stale color overwrote a changed original");
        require(read_appearance(db) == application::Appearance{application::Theme::dark, blue} && recent_colors(db).empty(),
                "Conflict changed color, theme or recents");
        sqlite3_commit_hook(db.handle(), [](void*) { return 1; }, nullptr);
        history_fixture::rejects([&] { save_match_color(db, blue, gold); }, "Rejected COMMIT reported success");
        sqlite3_commit_hook(db.handle(), nullptr, nullptr);
        require(read_appearance(db).match_color == blue && recent_colors(db).empty(), "Failed color commit leaked recents");
        save_match_color(db, blue, gold);
        require(read_appearance(db) == application::Appearance{application::Theme::dark, gold} &&
                recent_colors(db) == std::vector{*gold.custom}, "Retry lost theme, color or recents");
    }
    HistoryStore reopened(profile.path);
    auto& db = reopened.database();
    require(read_appearance(db).match_color == gold, "Restart lost the selected color");
    save_match_color(db, gold, {});
    require(read_appearance(db).match_color.is_default() && recent_colors(db) == std::vector{*gold.custom},
            "Default added a recent color");
    reset_global_preferences(db);
    require(read_appearance(db) == application::Appearance{} && recent_colors(db).empty(), "Reset retained a color");
    std::cout << "Match color CAS, failed commit, retry, preserved theme, recents, restart and reset passed\n";
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--match-color") {
        try { match_color(); return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    auto root =
        std::filesystem::current_path() / ("ClipRope-colors-" + std::to_string(GetCurrentProcessId()));
    try {
        {
            HistoryStore store(root);
            Browser browser(store);
            auto& db = store.database();
            domain::Format text;
            text.standard_id = CF_UNICODETEXT;
            text.bytes = {'A',0,0,0};
            const auto entry = store.admit("main", {{text}});
            const auto rope = store.create_rope("Tinted", 5);
            store.save_rope_color(rope, {}, domain::ColorSelection(0x123456));
            require(browser.rope(rope).material == domain::RopeMaterial::tinted(0x123456),
                    "Saved tint lost its material");
            {
                Transaction tx(db.handle());
                for (unsigned i = 0; i < 10; ++i)
                    remember_color(db.handle(), domain::ColorSelection(i));
                tx.commit();
            }
            require(recent_colors(db).size() == 8 && recent_colors(db).front().rgb() == 9,
                    "Recent capacity/order");
            {
                Transaction tx(db.handle());
                remember_color(db.handle(), domain::ColorSelection(100));
            }
            require(recent_colors(db).front().rgb() == 9, "Recent rollback failed");
            const auto target = store.entry(entry);
            const auto style = browser.summary(entry).style;
            const domain::ColorSelection white(0xffffff), black(0), tint(domain::ColorRGBA8{20, 80, 210, 96});
            store.save_color(target, true, style.background_color, tint);
            store.save_color(target, false, style.text_color, white);
            auto changed = browser.summary(entry);
            require(changed.style.background_color == tint && changed.style.text_color == white &&
                        changed.entry.snapshot_id == target.snapshot_id &&
                        changed.entry.revision == target.revision &&
                        changed.entry.sort_key == target.sort_key,
                    "Color changed another field, payload or age");
            history_fixture::rejects([&] { store.save_color(target, false, style.text_color, black); },
                                     "Stale color overwrote a newer field");
            require(browser.summary(entry).style.text_color == white, "Conflict changed color");
            exec(db.handle(), "CREATE TRIGGER reject_color BEFORE UPDATE OF paper_tint ON card_presentation "
                              "BEGIN SELECT RAISE(ABORT,'injected color failure'); END");
            const auto before_recent = recent_colors(db);
            history_fixture::rejects([&] { store.save_color(target, true, tint, white); },
                                     "Injected failure accepted");
            require(browser.summary(entry).style.background_color == tint && recent_colors(db) == before_recent,
                    "Failed save changed color or recents");
            exec(db.handle(), "DROP TRIGGER reject_color");
            store.save_color(target, true, tint, {});
            require(browser.summary(entry).style.background_color.is_default() &&
                        recent_colors(db) == before_recent,
                    "Default recorded as a recent color");
            const auto copy = store.copy(entry, "main");
            require(browser.summary(copy).style.text_color == white &&
                        browser.summary(copy).style.background_color.is_default(),
                    "Copy lost color identity");
            store.erase(copy);
            auto missing = target;
            missing.id = copy;
            history_fixture::rejects([&] { store.save_color(missing, false, white, black); },
                                     "Deleted target accepted");
            store.save_rope_color(rope, browser.rope(rope).color, black);
            require(browser.rope(rope).material == domain::RopeMaterial::tinted(0) &&
                        !browser.rope(rope).material.natural,
                    "New black became natural rope");
            history_fixture::rejects([&] { store.save_rope_color(rope, 0x123456, white); },
                                     "Stale rope accepted");
            store.save_rope_color(rope, black, {});
            require(browser.rope(rope).color.is_default() &&
                        browser.rope(rope).material == domain::RopeMaterial{},
                    "Default rope did not restore its material");
            db.backup_to(root.string() + "-backup");
        }
        {
            HistoryStore reopened(root);
            require(Browser(reopened).rope("main").material == domain::RopeMaterial{},
                    "Reopen changed material");
        }
        Database::restore_backup(root.string() + "-backup", root.string() + "-restored");
        {
            HistoryStore restored(root.string() + "-restored");
            require(recent_colors(restored.database()).size() == 8, "Backup lost colors");
        }
        std::cout << "Color persistence, material, recent rollback and restore passed\n";
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(root.string() + "-backup");
        std::filesystem::remove_all(root.string() + "-restored");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
