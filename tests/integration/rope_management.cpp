#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/browse.hpp"
#include <iostream>

namespace {
using namespace cliprope;
using application::RopeChange;
using application::RopeOp;
using history_fixture::rejects;
using history_fixture::require;
using persistence::HistoryStore;
domain::Snapshot sample(unsigned char byte) {
    domain::Format f;
    f.name = "Fixture Binary";
    f.bytes = {byte};
    return {{f}};
}
std::vector<domain::Id> ids(HistoryStore &store, const std::string &rope) {
    std::vector<domain::Id> result;
    for (const auto &e : store.hanging(rope))
        result.push_back(e.id);
    return result;
}
void gesture_cases(const std::filesystem::path &profile) {
    bool fail = false;
    std::string b, c, card;
    {
        HistoryStore s(profile, [&](auto point) {
            if (fail && point == "transaction") throw std::runtime_error("Simulated commit failure");
        });
        b = s.create_rope("B", 3);
        c = s.create_rope("C", 3);
        card = s.admit("main", sample(1));
        s.set_active_rope(b);
        s.save_rope_top("main", 60);
        s.save_rope_top(b, 120.5);
        s.save_rope_top(c, 180);
        // The visible order is B, C, main. Move main between B and C.
        require(s.reorder_rope("main", c, b) == b, "Non-head insertion changed the collector");
        auto rows = persistence::Browser(s).ropes(0, 3);
        require(rows[0].id == b && rows[1].id == "main" && rows[2].id == c, "Circular insertion used saved indices");
        fail = true;
        rejects([&] { s.reorder_rope(c, b, b); }, "Failed reorder was committed");
        rejects([&] { s.save_rope_top(b, 250); }, "Failed placement was committed");
        fail = false;
        rows = persistence::Browser(s).ropes(0, 3);
        require(s.active_rope() == b && rows[0].id == b && rows[1].id == "main" && rows[2].id == c,
                "Failed insertion changed the order or collector");
        rejects([&] { s.reorder_rope("main", "missing", b); }, "Missing target accepted");
        rejects([&] { s.reorder_rope("main", c, c); }, "Stale collector accepted");
        require(s.reorder_rope(c, b, b) == c && s.active_rope() == c, "New head did not become collector");
    }
    HistoryStore s(profile);
    auto rows = persistence::Browser(s).ropes(0, 3);
    require(rows[0].id == c && rows[1].id == b && rows[2].id == "main" &&
            s.active_rope() == c && ids(s, "main") == std::vector{card}, "Rope placement changed history");
    require(s.rope_top("main") == 60 && s.rope_top(b) == 120.5 && s.rope_top(c) == 180,
            "Independent heights did not survive restart");
}
void store_cases(const std::filesystem::path &profile) {
    bool fail = false;
    std::string source, dest, floating;
    std::vector<domain::Id> wanted;
    {
        HistoryStore s(profile, [&](std::string_view where) {
            if (fail && where == "transaction")
                throw std::runtime_error("Injected rollback");
        });
        auto apply = [&](RopeChange c) { return s.apply_rope_change(s.preview_rope_change(c)); };
        RopeChange main{RopeOp::properties, "main"};
        main.name = "Changed";
        main.capacity = 100;
        rejects([&] { apply(main); }, "Main rename passed the command boundary");
        main.name = "main";
        main.color = 0xabcdef;
        apply(main);
        require(persistence::Browser(s).rope("main").color == main.color, "Main properties became read-only");
        RopeChange defaults{RopeOp::defaults};
        defaults.capacity = 3;
        apply(defaults);
        require(s.default_capacity() == 3, "Default capacity failed");
        RopeChange create{RopeOp::create};
        create.capacity = s.default_capacity();
        create.name = "A";
        const auto old_count = persistence::Browser(s).rope_count();
        fail = true;
        rejects([&] { apply(create); }, "Failed creation committed");
        fail = false;
        require(s.active_rope() == "main" && persistence::Browser(s).rope_count() == old_count,
                "Failed creation changed collector or rope count");
        source = apply(create).value();
        require(s.active_rope() == "main", "Creation selected the new rope before confirmation");
        create.name = "B";
        dest = apply(create).value();
        fail = true;
        rejects([&] { s.set_active_rope(dest); }, "Failed selection committed");
        fail = false;
        require(s.active_rope() == "main" && persistence::Browser(s).rope_count() == old_count + 2,
                "Failed selection removed the created rope or changed collector");
        auto a = s.admit(source, sample(1)), b = s.admit(source, sample(2));
        floating = s.admit(source, sample(3));
        s.detach(floating);
        s.admit(dest, sample(4));
        s.admit(dest, sample(5));
        auto top = s.admit(dest, sample(6));
        s.set_active_rope(source);
        RopeChange transfer{RopeOp::erase, source, dest};
        auto preview = s.preview_rope_change(transfer);
        require(preview.moved == 2 && preview.overflow == 2 && preview.hanging == 2 && preview.floating == 0,
                "Bad transfer counts");
        s.reorder(a, 0);
        rejects([&] { s.apply_rope_change(preview); }, "Accepted stale order at unchanged counts");
        auto before = ids(s, dest);
        wanted = {a, b, top};
        preview = s.preview_rope_change(transfer);
        fail = true;
        rejects([&] { s.apply_rope_change(preview); }, "Injected transfer committed");
        fail = false;
        require(ids(s, dest) == before && ids(s, source) == std::vector<std::string>{a, b} &&
                    s.active_rope() == source,
                "Rollback lost original orders or collector");
        s.apply_rope_change(preview);
        require(ids(s, dest) == wanted && s.active_rope() == "main" && !s.entry(floating).rope_id,
                "Transfer or active fallback failed");
        rejects([&] { s.preview_rope_change({RopeOp::erase, "main"}); }, "Main rope can be deleted");
    }
    HistoryStore reopened(profile);
    require(ids(reopened, dest) == wanted && reopened.floating()[0].id == floating,
            "Reopened transfer changed membership");
    RopeChange shrink{RopeOp::properties, dest};
    shrink.name = "Renamed";
    shrink.color = 0x1743a1;
    shrink.capacity = 1;
    auto preview = reopened.preview_rope_change(shrink);
    require(preview.shrink && preview.hanging == 2, "Bad shrink preview");
    reopened.reorder(wanted.back(),0);
    const auto reordered=ids(reopened,dest);
    const auto old_capacity=persistence::Browser(reopened).rope(dest).capacity;
    rejects([&]{reopened.apply_rope_change(preview);},"Stale shrink accepted after tail identities changed");
    require(ids(reopened,dest)==reordered && persistence::Browser(reopened).rope(dest).capacity==old_capacity,
            "Stale shrink changed capacity or order");
    wanted=reordered;
    preview=reopened.preview_rope_change(shrink);
    reopened.apply_rope_change(preview);
    require(ids(reopened, dest) == std::vector<std::string>{wanted[0]} &&
                persistence::Browser(reopened).rope(dest).color == shrink.color,
            "Wrong shrink suffix or color");
    auto clear = reopened.preview_rope_change({RopeOp::clear_all});
    reopened.admit("main", sample(7));
    rejects([&] { reopened.apply_rope_change(clear); },
            "Newly admitted entry was cleared without renewed consent");
    reopened.apply_rope_change(reopened.preview_rope_change({RopeOp::clear, dest}));
    reopened.apply_rope_change(reopened.preview_rope_change({RopeOp::clear_all}));
    require(reopened.floating().size() == 1 && reopened.hanging("main").empty(),
            "Clear scopes destroyed Floating content");
    auto all = reopened.preview_rope_change({RopeOp::delete_all});
    require(all.floating == 1 && all.hanging == 0, "Delete-all counts incorrect");
    reopened.apply_rope_change(all);
    require(reopened.floating().empty(), "Delete-all omitted Floating");
    std::cout << "stale_same_count_rejected=1 atomic_transfer=1 source_order_preserved=1 target_tail=2 "
                 "floating_survived=1\n";
}
void rebin_cases(const std::filesystem::path& profile) {
    const domain::Id bin = domain::rebin_id;
    domain::Id a, b, first, last;
    bool fail = false;
    {
        HistoryStore s(profile, [&](auto point) {
            if (fail && point == "transaction") throw std::runtime_error("Injected rollback");
        });
        persistence::Browser browser(s);
        a = s.create_rope("ReBin", 2);
        b = s.create_rope("B", 2);
        s.set_active_rope(a);
        require(browser.rope_count() == 3 && browser.rope_count(true) == 4 && !browser.rope_position(bin) &&
                !browser.rope(bin).capacity, "Hidden ReBin leaked into switching order");
        rejects([&] { s.set_active_rope(bin); }, "Selected a hidden ReBin");
        rejects([&] { s.reorder_rope(bin, a, a); }, "Reordered a hidden ReBin");
        rejects([&] { s.erase_rope(bin); }, "Removed ReBin");
        rejects([&] { s.set_capacity(bin, 1); }, "Assigned a finite ReBin capacity");
        for (int i = 0; i <= 100; ++i) {
            last = s.admit(a, sample(1));
            s.detach(last);
            s.rehang(last, bin);
            if (i == 0) first = last;
        }
        const auto recycled = ids(s, bin);
        require(recycled.size() == 101 && recycled.front() == last && recycled.back() == first &&
                s.active_rope() == a && !s.rebin_visible(), "Recycle truncated or revealed ReBin");
        const auto copy = s.copy(last, bin, nullptr, 101);
        const auto outside = s.admit(b, sample(2));
        s.move(outside, bin, nullptr, 102);
        require(ids(s, bin).back() == outside && s.entry(copy).snapshot_id == s.entry(last).snapshot_id,
                "Unbounded drop slot or shared copy lost");
        fail = true;
        rejects([&] { s.set_rebin_visible(true); }, "Failed reveal committed");
        fail = false;
        require(!s.rebin_visible() && s.active_rope() == a, "Failed reveal changed session state");
        s.set_rebin_visible(true);
        require(s.active_rope() == bin && browser.rope_position(bin).value() + 1 == browser.rope_position(a),
                "ReBin did not precede its former collector");
        require(s.reorder_rope(b, bin, bin) == b, "Visible ReBin rejected an ordinary reorder");
        s.set_active_rope(bin);
        s.set_rebin_visible(false);
        require(s.active_rope() == b && !browser.rope_position(bin), "Hide lost recent collector");
        s.set_rebin_visible(true);
        s.erase_rope(b);
        require(s.active_rope() == bin, "Deleting the fallback interrupted ReBin");
    }
    HistoryStore restarted(profile);
    require(!restarted.rebin_visible() && restarted.active_rope() == "main" && ids(restarted, bin).size() == 103,
            "Restart lost recycled history or the main fallback");
    restarted.erase(first);
    restarted.apply_rope_change(restarted.preview_rope_change({RopeOp::clear, bin}));
    require(ids(restarted, bin).empty(), "Manual cleanup left recycled cards");
}
void density_cases(const std::filesystem::path& profile) {
    bool fail = false;
    domain::Id other, card;
    {
        HistoryStore store(profile, [&](auto point) {
            if (fail && point == "transaction") throw std::runtime_error("Simulated density failure");
        });
        other = store.create_rope("Regular", 10);
        card = store.admit("main", sample(1));
        require(!store.rope_compact("main") && !store.rope_compact(other), "New rope is not regular");
        store.save_rope_compact("main", true, {card, 30});
        fail = true;
        rejects([&] { store.save_rope_compact("main", false, {card, 60}); }, "Failed density write committed");
        fail = false;
        const auto anchor = persistence::Browser(store).load_anchor("main");
        require(store.rope_compact("main") && anchor.entry == card && anchor.offset == 30 &&
                !store.rope_compact(other), "Density failure changed one half of the view state");
        rejects([&] { store.save_rope_compact("missing", true, {}); }, "Missing rope accepted density");
    }
    HistoryStore reopened(profile);
    const auto anchor = persistence::Browser(reopened).load_anchor("main");
    require(reopened.rope_compact("main") && !reopened.rope_compact(other) && anchor.entry == card &&
            anchor.offset == 30 && reopened.active_rope() == "main", "Density or anchor lost on restart");
}
void name_cases(const std::filesystem::path& profile) {
    const std::string han="\xe6\xb1\x89", emoji="\xf0\x9f\x98\x80";
    auto repeat=[](const std::string& item, int count) { std::string out; while(count-->0) out+=item; return out; };
    domain::Id id;
    {
        HistoryStore store(profile);
        persistence::Browser records(store);
        id=store.create_rope("ordinary",4);
        auto change=[&](const std::string& name) {
            RopeChange rename{RopeOp::properties,id};
            rename.name=name;
            rename.capacity=4;
            store.apply_rope_change(store.preview_rope_change(rename));
        };
        for(const auto& name:{repeat(han,85),repeat(han,86),repeat(han,256),repeat(emoji,128),std::string(256,'a')}) {
            const auto created=store.create_rope(name,4);
            change(name);
            require(records.rope(created).name==name && records.rope(id).name==name,"Valid Unicode name changed bytes");
        }
        auto rejects_name=[](auto command) {
            try { command(); }
            catch (const application::InvalidRopeName&) { return; }
            throw std::runtime_error("Invalid name did not receive its specific rejection");
        };
        for(const auto& name:{repeat(han,257),repeat(emoji,129),std::string(257,'a'),std::string{},
                              std::string(" \t\r\n"),std::string("\xe3\x80\x80"),std::string("x\0y",3),
                              std::string("\xed\xa0\x80"),std::string("\xc0\xaf")}) {
            rejects_name([&]{store.create_rope(name,4);});
            RopeChange create{RopeOp::create}; create.name=name; create.capacity=4;
            rejects_name([&]{store.preview_rope_change(create);});
            rejects_name([&]{change(name);});
        }
    }
    HistoryStore reopened(profile);
    persistence::Browser records(reopened);
    require(records.rope(id).name==std::string(256,'a') && records.rope("main").name=="main" && records.rope(domain::rebin_id).name=="ReBin",
            "Restart changed a Unicode or reserved name");
    persistence::Statement corrupt(reopened.database().handle(), "UPDATE ropes SET name=CAST(x'E08080' AS TEXT) WHERE id=?");
    corrupt.bind(1, id);
    corrupt.run();
    rejects([&]{records.rope(id);}, "Displayed a persisted name with invalid UTF-8");
}
} // namespace
int main() {
    try {
        history_fixture::Directory tmp;
        store_cases(tmp.path);
        gesture_cases(tmp.path / "gesture-store");
        rebin_cases(tmp.path / "rebin-store");
        density_cases(tmp.path / "density-store");
        name_cases(tmp.path / "names");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
