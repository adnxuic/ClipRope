#include "persistence/history_store.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace cliprope;
using persistence::HistoryStore;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

domain::Snapshot sample(unsigned char letter) {
    domain::Format text;
    text.standard_id = 13;
    text.bytes = {letter, 0, 0, 0};
    domain::Snapshot data;
    data.formats.push_back(std::move(text));
    return data;
}

void order(HistoryStore& store, const domain::Id& rope, std::vector<domain::Id> expected) {
    std::vector<domain::Id> actual;
    for (auto& item : store.hanging(rope)) {
        require(item.rope_id == rope && item.sort_key.has_value(), "Invalid Hanging shape");
        actual.push_back(item.id);
    }
    require(actual == expected, "Incorrect display order after command");
}

template<class F>
void rejected(F command) {
    try { command(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("A rejected command unexpectedly succeeded");
}

void compare_capacity_paths(const fs::path& root) {
    const auto direct = root / "direct", bulk = root / "bulk";
    domain::Id f, a, b;
    {
        HistoryStore seed(direct);
        seed.set_capacity("main", 2);
        b = seed.admit("main", sample('B'));
        a = seed.admit("main", sample('A'));
        f = seed.create_floating_text({"",40,50,240,160});
    }
    fs::copy(direct, bulk, fs::copy_options::recursive);
    for (const auto& profile : {direct, bulk}) {
        bool reject{};
        {
            HistoryStore store(profile, [&](auto boundary) {
                if (reject && boundary == "transaction") throw std::runtime_error("Injected capacity rollback");
            });
            if (profile == bulk) {
                store.set_capacity("main", 3);
                store.rehang(f, "main");
            }
            auto commit = [&] {
                if (profile == direct) store.rehang(f, "main");
                else store.set_capacity("main", 2);
            };
            reject = true;
            rejected(commit);
            if (profile == direct) {
                order(store, "main", {a,b});
                require(!store.entry(f).rope_id, "Rejected return lost its Floating entry");
            } else order(store, "main", {f,a,b});
            reject = false;
            commit();
        }
        HistoryStore reopened(profile);
        order(reopened, "main", {f,a});
        rejected([&] { reopened.entry(b); });
    }
}

void exercise(const fs::path& root) {
    domain::Id a, b, c, d, x, y, target, other, snapshot_c, copy;
    {
        bool fail = false;
        HistoryStore store(root, [&](std::string_view point) {
            if (fail && point == "transaction") throw std::runtime_error("Injected rollback");
        });
        require(store.active_rope() == "main", "Fresh store has no active main rope");
        store.set_capacity("main", 3);
        c = store.admit("main", sample('C'));
        b = store.admit("main", sample('B'));
        a = store.admit("main", sample('A'));
        store.promote(c);
        d = store.admit("main", sample('D'));
        order(store, "main", {d, c, a});
        rejected([&] { store.entry(b); });
        target = store.create_rope("Target", 2);
        other = store.create_rope("Other", 3);
        x = store.admit(target, sample('X'));
        y = store.admit(target, sample('Y'));
        snapshot_c = store.entry(c).snapshot_id;
        store.detach(c, {"monitor", -20, 15, 200, 120});
        order(store, "main", {d, a});
        store.set_active_rope(target);
        fail = true;
        rejected([&] { store.rehang_at(c, target, 1, nullptr); });
        rejected([&] { store.move(a, target, nullptr, 1); });
    }
    {
        HistoryStore store(root);
        order(store, "main", {d, a});
        order(store, target, {y, x});
        const auto floating = store.floating();
        require(floating.size() == 1 && floating.front().id == c && !floating.front().rope_id &&
                !floating.front().sort_key && floating.front().snapshot_id == snapshot_c, "Failed return lost Floating card");
        require(store.layout(c).x == -20 && store.layout(c).monitor == "monitor", "Floating layout was lost");
        require(store.active_rope() == target, "Target switch failed to persist");
        store.set_active_rope(other);
        store.rehang(c, target); // Previously bound target survives a collector switch.
        order(store, target, {c, y});
        require(store.floating().empty() && store.entry(c).snapshot_id == snapshot_c, "Return changed identity");
        rejected([&] { store.entry(x); });
        store.move(a, target);
        order(store, "main", {d});
        order(store, target, {a, c});
        rejected([&] { store.entry(y); });
        copy = store.copy(c, other);
        require(copy != c && store.entry(copy).snapshot_id == snapshot_c, "Copy did not share content");
        store.set_capacity(target, 1);
        order(store, target, {a});
        rejected([&] { store.entry(c); });
        store.gc();
        require(store.load(copy).snapshot.formats.at(0).bytes == sample('C').formats[0].bytes, "Copy lost shared bytes");
        const auto p = store.admit(other, sample('P'));
        const auto q = store.admit(other, sample('Q'));
        store.reorder(q, 2);
        const auto n = store.admit(other, sample('N'));
        order(store, other, {n, p, copy});
        rejected([&] { store.entry(q); });
        store.detach(copy);
        store.erase_rope(other);
        require(store.active_rope() == "main", "Deleting active rope did not switch to main");
        rejected([&] { store.rehang(copy, other); });
        rejected([&] { store.erase_rope("main"); });
        order(store, "main", {d});
    }
    {
        HistoryStore store(root);
        order(store, "main", {d});
        order(store, target, {a});
        require(store.floating().size() == 1 && !store.entry(copy).rope_id, "A rope deletion removed Floating");
        store.erase(copy);
        store.gc();
        require(store.floating().empty(), "Explicit Floating delete failed");
    }
}

void observer_failure(const fs::path& root) {
    domain::Id head, tail, floating, snapshot;
    const domain::Layout layout{"monitor", 20, 40, 240, 160};
    bool reject{};
    int commits{}, notices{};
    {
        HistoryStore store(root, [&](std::string_view point) {
            if (point == "transaction" && reject) throw std::runtime_error("Injected rollback");
            if (point == "committed") ++commits;
        });
        store.set_capacity("main", 2);
        floating = store.admit("main", sample('F'));
        store.detach(floating, layout);
        tail = store.admit("main", sample('B'));
        head = store.admit("main", sample('A'));
        snapshot = store.entry(floating).snapshot_id;
        commits = 0;
        store.on_head_change = [&](const auto&) { ++notices; throw std::runtime_error("Injected observer failure"); };
        reject = true;
        rejected([&] { store.rehang(floating, "main"); });
        order(store, "main", {head, tail});
        require(!store.entry(floating).rope_id && store.layout(floating).x == layout.x &&
                !commits && !notices, "Failed return changed layout or notified an observer");
        reject = false;
        store.rehang(floating, "main");
        order(store, "main", {floating, head});
        require(commits == 1 && notices == 1 && store.failed_head_notifications() == 1,
                "Return notification did not report its failure independently");
    }
    {
        HistoryStore store(root);
        order(store, "main", {floating, head});
        rejected([&] { store.entry(tail); });
    }
    {
        HistoryStore store(root, [&](std::string_view point) {
            if (point == "transaction" && reject) throw std::runtime_error("Injected rollback");
            if (point == "committed") ++commits;
        });
        store.on_head_change = [&](const auto&) { ++notices; throw std::runtime_error("Injected observer failure"); };
        reject = true;
        rejected([&] { store.detach(floating, layout); });
        order(store, "main", {floating, head});
        require(commits == 1 && notices == 1, "Failed detach notified an observer");
        reject = false;
        store.detach(floating, layout);
        require(commits == 2 && notices == 2 && store.failed_head_notifications() == 1,
                "Detach notification did not report its failure independently");
    }
    HistoryStore reopen(root);
    order(reopen, "main", {head});
    const auto card = reopen.entry(floating);
    require(!card.rope_id && !card.sort_key && card.snapshot_id == snapshot &&
            reopen.layout(floating).monitor == layout.monitor, "Committed detach did not survive restart");
}
} // namespace

int main(int argc, char** argv) {
    const auto root = fs::temp_directory_path() /
        ("ClipRope-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(fs::create_directory(root), "Failed to create fresh history fixture");
        if (argc == 2 && std::string_view(argv[1]) == "--observer") observer_failure(root);
        else {
            exercise(root);
            compare_capacity_paths(root / "capacity-parity");
        }
        fs::remove_all(root);
        std::cout << "Verified atomic promotion, reorder, detach/return, rollback, move/copy and shrink after reopen\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
