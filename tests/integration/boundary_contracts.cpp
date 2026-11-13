#include "history_fixture.hpp"
#include "cache/thumbnail_name.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/file_group.hpp"
#include "presentation/card_capabilities.hpp"
#include "presentation/card_labels.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include <fstream>
#include <iostream>

namespace {
using namespace cliprope;
using history_fixture::require;
using persistence::HistoryStore;
namespace fs = std::filesystem;

void formats() {
    using application::CardFormat;
    presentation::CardCapabilities facts(false);
    std::vector<CardFormat> expected;
    for (int i = 0; i < static_cast<int>(CardFormat::count); ++i) {
        const auto type = static_cast<CardFormat>(i);
        expected.push_back(type);
        facts.retain(type);
        facts.retain(type);
    }
    for (auto invalid : {static_cast<CardFormat>(-1), CardFormat::count}) {
        history_fixture::rejects([&] { facts.retain(invalid); }, "Invalid format entered the capability set");
    }
    application::CardSummary card;
    facts.apply(card);
    presentation::localize_card(card, presentation::english_labels());
    require(card.formats == expected && !card.capabilities.empty(), "Capability order or labels changed");
}

void put(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file.put('x');
    require(file.good(), "Could not seed thumbnail fixture");
}

void thumbnail_files() {
    history_fixture::Directory dir;
    const auto folder = dir.path / "thumbnails";
    fs::create_directory(folder);
    const auto png = media_sample::png();
    const auto hash = persistence::sha256(png.bytes);
    const auto group = encode_file_group({(dir.path / "one.png").wstring()});
    const auto list_hash = persistence::sha256(group);
    std::vector<fs::path> live, garbage, foreign;
    domain::Id image, list;
    {
        HistoryStore store(dir.path);
        image = store.admit("main", {{{0, "PNG", png.bytes}}});
        list = store.admit("main", {{{CF_HDROP, {}, group}}});
        for (auto adapter : {cache::png, cache::dib, cache::dibv5, cache::files}) {
            for (unsigned width : cache::thumbnail_widths) {
                const bool is_list = adapter == cache::files;
                const auto& id = is_list ? list_hash : hash;
                const auto key = cache::thumbnail_key(id, adapter, width);
                const auto name = key + ".thumb";
                auto parsed = cache::parse_thumbnail_name(name);
                require(parsed && parsed->hash == id && parsed->file_list == is_list && !parsed->staged,
                        "Writer and collector disagree on a thumbnail name");
                live.push_back(folder / name);
                garbage.push_back(folder / (name + ".tmp"));
            }
        }
        require(cache::thumbnail_width(0) == 80 && cache::thumbnail_width(81) == 160 &&
                cache::thumbnail_width(1025) == 1024, "Thumbnail width buckets changed");
        require(cache::thumbnail_key(hash, cache::png, 320) == hash + "-png", "Legacy cache key changed");
        live.push_back(folder / (hash + "-png-320.thumb")); // Older sized spelling is also recognized.
        garbage.push_back(folder / (hash + "-files-320.thumb"));
        garbage.push_back(folder / (list_hash + "-png.thumb"));
        for (auto suffix : {"-unknown.thumb", "-files.thumb", "-png-079.thumb.tmp", "-png-81.thumb",
                            "-png-80.thumb.tmp.tmp", "-png-1025.thumb", "-status.thumb"})
            foreign.push_back(folder / (hash + suffix));
        foreign.push_back(folder / ("bad-digest-png.thumb.tmp"));
        for (const auto& files : {&live, &garbage, &foreign}) for (const auto& file : *files) put(file);
    }
    {
        HistoryStore reopened(dir.path);
        for (const auto& file : live) require(fs::exists(file), "Reachable derivative removed at reopen");
        for (const auto& file : garbage) require(!fs::exists(file), "Recognized garbage survived reopen");
        reopened.erase(image);
        reopened.erase(list);
        reopened.gc();
        for (const auto& file : live) require(!fs::exists(file), "Unreachable derivative survived GC");
        for (const auto& file : foreign) require(fs::exists(file), "Foreign thumbnail name removed");
    }
}

enum class Route { admit, rehang, shrink, merge, expand };

void victim(Route route) {
    history_fixture::Directory dir;
    const domain::Snapshot text{{{CF_UNICODETEXT, {}, {'A',0,0,0}}}};
    std::vector<domain::Id> expected;
    domain::Id tail;
    auto verify = [&](HistoryStore& store) {
        std::vector<domain::Id> actual;
        for (const auto& entry : store.hanging("main")) actual.push_back(entry.id);
        require(actual == expected, "Capacity route changed a survivor or its position");
        history_fixture::rejects([&] { store.entry(tail); }, "Wrong tail retained");
    };
    {
        HistoryStore store(dir.path);
        store.set_capacity("main", route == Route::shrink ? 4 : 3);
        tail = store.admit("main", text);
        const auto survivor = store.admit("main", text);
        if (route == Route::expand) {
            const auto paths = encode_file_group({(dir.path / "a").wstring(), (dir.path / "b").wstring()});
            const auto parent = store.admit("main", {{{CF_HDROP, {}, paths}}});
            const auto group = store.prepare_expansion(store.entry(parent), false);
            const auto impact = store.preview_expansion(group);
            require(impact.removed_tail == std::vector<domain::Id>{tail}, "Consent named the wrong victim");
            expected = {group.members[0].id, group.members[1].id, survivor};
            require(store.expand_file_group(group, impact), "Reviewed expansion rejected");
        } else {
            const auto head = store.admit("main", text);
            domain::Id incoming;
            if (route == Route::rehang || route == Route::merge) {
                const auto rope = store.create_rope("Source", 1);
                incoming = store.admit(rope, text);
                if (route == Route::rehang) {
                    store.detach(incoming);
                    store.rehang(incoming, "main");
                } else {
                    auto review = store.preview_rope_change({application::RopeOp::erase, rope, "main"});
                    require(review.overflow == 1, "Merge preview omitted the tail");
                    store.apply_rope_change(review);
                }
            } else {
                incoming = store.admit("main", text);
                if (route == Route::shrink) store.set_capacity("main", 3);
            }
            expected = {incoming, head, survivor};
        }
        verify(store);
    }
    HistoryStore reopened(dir.path);
    verify(reopened);
}
}

int main() {
    try {
        formats();
        thumbnail_files();
        for (auto route : {Route::admit, Route::rehang, Route::shrink, Route::merge, Route::expand})
            victim(route);
        std::cout << "Format bounds, cache ownership and five reopened capacity routes passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
