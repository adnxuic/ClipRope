#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "persistence/card_search.hpp"
#include "platform_win/file_group.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace cliprope;
using history_fixture::require;
namespace fs = std::filesystem;
namespace {
domain::Snapshot group_snapshot(const std::vector<std::wstring>& paths) {
    domain::Format drop, companion;
    drop.standard_id = CF_HDROP;
    drop.bytes = encode_file_group(paths);
    companion.standard_id = CF_UNICODETEXT;
    companion.bytes = {'X',0,0,0};
    return {{drop, companion}};
}

void run() {
    history_fixture::Directory dir;
    const auto folder = dir.path / "folder.png", picture = dir.path / "one.png";
    fs::create_directory(folder);
    const auto missing = folder / "one.png";
    std::ofstream(picture).put('x');
    const std::vector<std::wstring> paths{folder, picture, missing, picture};
    domain::Id original, rope;
    std::vector<application::FileGroupMember> members;
    {
        bool fail = false;
        persistence::HistoryStore store(dir.path, [&](auto point) {
            if (fail && point == "transaction") throw std::runtime_error("Injected expansion failure");
        });
        rope = store.create_rope("Group", 6);
        auto snapshot = group_snapshot(paths);
        const auto last = store.admit(rope, snapshot), other = store.admit(rope, snapshot);
        original = store.admit(rope, snapshot);
        const auto ahead = store.admit(rope, snapshot);
        const auto group = inspect_expansion(store.prepare_expansion(store.entry(original)));
        members = group.members;
        auto impact = store.preview_expansion(group);
        require(impact.removed_tail == std::vector<domain::Id>{other,last}, "Incorrect eviction preview");
        store.reorder(last, 0);
        require(!store.expand_file_group(group, impact) && store.hanging(rope).size() == 4,
                "Stale consent changed history");
        require(store.preview_expansion(group).required_capacity == 7, "Missing minimum capacity");
        store.reorder(last, 3);
        store.reorder(other, 1);
        store.reorder(original, 1);
        store.set_capacity(rope, 7);
        impact = store.preview_expansion(group);
        require(impact.removed_tail == std::vector<domain::Id>{last}, "Incorrect revised eviction");
        store.reorder(other, 3); // Same count, different victim.
        require(!store.expand_file_group(group, impact), "Consent was reused for a different victim");
        impact = store.preview_expansion(group);
        fail = true;
        history_fixture::rejects([&] { store.expand_file_group(group, impact); }, "Injected failure committed");
        fail = false;
        require(store.entry(other).id == other && store.hanging(rope).size() == 4, "Rollback lost the tail");
        require(store.expand_file_group(group, impact), "Confirmed expansion failed");
        const auto entries = store.hanging(rope);
        require(entries.size() == 7 && entries[0].id == ahead && entries[1].id == original &&
                entries.back().id == last && store.active_rope() == "main", "Wrong destination or order");
        require(store.load(original).snapshot.formats.at(0).bytes == snapshot.formats[0].bytes,
                "Expansion replaced the original group");
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const auto& entry = entries.at(i + 2);
            const auto formats = store.load(entry.id).snapshot.formats;
            require(entry.id == members.at(i).id && formats.size() == 2 && formats[0].standard_id == CF_HDROP &&
                    file_group_paths(formats[0].bytes) == std::vector<std::wstring>{paths[i]} &&
                    formats[1].name == "Preferred DropEffect" && formats[1].bytes == copy_drop_effect().bytes,
                    "Child identity, path order or copy-only formats incorrect");
        }
        store.detach(original);
        history_fixture::rejects([&] { store.expand_file_group(group, impact); }, "Changed membership accepted");
        store.rehang(original, rope);
        require(store.preview_expansion(inspect_expansion(store.prepare_expansion(store.entry(original)))).removed_tail.size() == 4,
                "Repeat did not prepare a fresh batch");
        const auto single = store.admit("main", group_snapshot({picture}));
        history_fixture::rejects([&] { inspect_expansion(store.prepare_expansion(store.entry(single))); }, "Single path expanded");
        const auto exact = store.create_rope("Exact fit", 5);
        const auto parent = store.admit(exact, snapshot);
        auto first = inspect_expansion(store.prepare_expansion(store.entry(parent)));
        require(store.expand_file_group(first, {}) && store.hanging(exact).size() == 5, "Exact fit rejected");
        auto second = inspect_expansion(store.prepare_expansion(store.entry(parent)));
        auto repeat = store.preview_expansion(second);
        require(repeat.removed_tail.size() == paths.size() && first.members[0].id != second.members[0].id &&
                store.expand_file_group(second, repeat) && store.hanging(exact)[1].id == second.members[0].id,
                "Repeat expansion did not create an independent batch");

        const auto replacement = store.create_rope("Replace", 4);
        const auto c = store.admit(replacement, snapshot), b = store.admit(replacement, snapshot);
        const auto source = store.admit(replacement, group_snapshot({folder, picture}));
        const auto a = store.admit(replacement, snapshot);
        auto keep = inspect_expansion(store.prepare_expansion(store.entry(source)));
        require(store.preview_expansion(keep).removed_tail == std::vector<domain::Id>{b,c}, "Keep preview incorrect");
        auto replace = inspect_expansion(store.prepare_expansion(store.entry(source), false));
        auto approved = store.preview_expansion(replace);
        require(approved.removed_tail == std::vector<domain::Id>{c}, "Replace preview counted the group");
        fail = true;
        history_fixture::rejects([&] { store.expand_file_group(replace, approved); }, "Replacement failed to roll back");
        fail = false;
        require(store.hanging(replacement)[1].id == source && store.hanging(replacement).back().id == c,
                "Failed replacement lost source or tail");
        require(store.expand_file_group(replace, approved), "Replacement failed");
        const auto replaced = store.hanging(replacement);
        require(replaced.size() == 4 && replaced[0].id == a && replaced[1].id == replace.members[0].id &&
                replaced[2].id == replace.members[1].id && replaced[3].id == b, "Incorrect replacement order");

        // The full list extends past the 16 KiB summary prefix.
        auto deep = paths;
        deep.resize(200, (dir.path / std::wstring(120,L't')).wstring());
        const auto id = store.admit(domain::rebin_id, group_snapshot(deep));
        const auto many = inspect_expansion(store.prepare_expansion(store.entry(id)));
        require(many.members.size() == deep.size() && store.preview_expansion(many) == application::ExpansionImpact{} &&
                store.expand_file_group(many, {}) && store.hanging(domain::rebin_id).size() == 201,
                "Unlimited expansion stopped at the summary prefix");
    }
    fs::remove(folder); // A directory recognized at expansion stays a folder card after it disappears.
    persistence::HistoryStore reopened(dir.path);
    rendering::CardBrowser browser{persistence::Browser(reopened)};
    require(browser.menu_summary(members[0].id).menu == application::CardMenuKind::folder &&
            browser.menu_summary(members[1].id).file_preview == application::CardSummary::FilePreviewKind::picture,
            "Reopened child did not use its single-member card type");
    persistence::CardSearch search(dir.path);
    const auto hits = search.query({rope}, L"one.png");
    std::vector<domain::Id> found;
    for (std::size_t i = 0; i < hits.total; ++i) found.push_back(search.locate(hits, i)->entry);
    for (const auto& member : members)
        if (member.path.ends_with(L"one.png"))
            require(std::find(found.begin(), found.end(), member.id) != found.end(), "Child filename not indexed");
}
}
int main() {
    try { run(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
