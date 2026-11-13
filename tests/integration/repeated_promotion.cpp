#include "history_observer.hpp"
#include "persistence/history_store.hpp"
#include <iostream>
#include <tuple>

namespace {
using namespace cliprope;
using namespace persistence;
using history_fixture::require;
using Rows = std::vector<std::pair<domain::Id, std::int64_t>>;

Rows keys(HistoryStore& store, std::string_view rope) {
    Rows rows;
    for (const auto& e : store.hanging(rope)) rows.emplace_back(e.id, e.sort_key.value());
    return rows;
}
Rows normalized(Rows rows) {
    std::int64_t index = 0;
    for (auto& [id, key] : rows) key = index++;
    return rows;
}
auto viewport(sqlite3* db) {
    Statement state(db, "SELECT version,anchor_entry_id,offset FROM rope_view_state WHERE rope_id='main'");
    require(state.step(), "Missing viewport");
    return std::tuple{state.integer(0), state.text(1), state.real(2)};
}
void set_key(sqlite3* db, std::string_view id, int key) {
    Statement write(db, "UPDATE entries SET sort_key=? WHERE id=?");
    write.bind(1, key);
    write.bind(2, id);
    write.run();
}

int traced_promote(HistoryStore& store, const domain::Entry& head) {
    auto* db = store.database().handle();
    int writes = 0;
    sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void* data, void*, void* text) {
        std::string_view sql(static_cast<const char*>(text));
        if (sql.starts_with("UPDATE entries SET sort_key")) ++*static_cast<int*>(data);
        return 0;
    }, &writes);
    struct Clear { sqlite3* db; ~Clear() { sqlite3_trace_v2(db, 0, nullptr, nullptr); } } clear{db};
    store.promote(head.id, &head);
    return writes;
}

void reorder_with_trigger(const std::filesystem::path& profile) {
    HistoryStore store(profile);
    domain::Format text;
    text.standard_id = 13;
    text.bytes = {'A',0,0,0};
    const auto first = store.admit("main", {{text}});
    for (int i = 0; i < 3; ++i) store.copy(first, "main");
    store.promote(store.hanging("main").front().id);
    auto expected = keys(store, "main");
    exec(store.database().handle(), "CREATE TEMP TRIGGER disturb_later_rank AFTER UPDATE OF sort_key ON entries "
         "WHEN OLD.sort_key=2 AND NEW.sort_key=1 BEGIN UPDATE entries SET sort_key=17 "
         "WHERE rope_id='main' AND sort_key=3; END");
    store.reorder(expected[2].first, 1);
    std::swap(expected[1], expected[2]);
    require(keys(store, "main") == normalized(expected), "A trigger changed a previously observed trailing rank");
}

void retry_ignored_key(const std::filesystem::path& profile) {
    HistoryStore store(profile);
    domain::Format text;
    text.standard_id = 13;
    text.bytes = {'A',0,0,0};
    const auto last = store.admit("main", {{text}});
    store.copy(last, "main");
    const auto head = store.entry(store.copy(last, "main"));
    store.promote(head.id);
    auto* db = store.database().handle();
    auto tail_key = [&] {
        Statement row(db, "SELECT typeof(sort_key),sort_key FROM entries WHERE id=?");
        row.bind(1, last);
        require(row.step() && row.text(0) == "integer", "Lost the tail entry or its integer sort key");
        return row.integer(1);
    };
    // entries is STRICT from v1 through v11. A failed assignment must preserve its integer value.
    const int result = sqlite3_exec(db, "UPDATE entries SET sort_key=1.5 WHERE rope_id='main' AND sort_key=2",
                                    nullptr, nullptr, nullptr);
    require((result & 255) == SQLITE_CONSTRAINT && sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_DATATYPE &&
            tail_key() == 2, "Strict history accepted a fractional sort key or changed the old value");
    set_key(db, last, 8);
    exec(db, "CREATE TEMP TABLE skip_version AS SELECT version FROM rope_view_state WHERE rope_id='main';"
             "CREATE TEMP TRIGGER skip_tail BEFORE UPDATE OF sort_key ON entries "
             "WHEN OLD.sort_key=8 AND (SELECT version FROM rope_view_state WHERE rope_id='main')="
             "(SELECT version FROM skip_version) BEGIN SELECT RAISE(IGNORE); END;");
    require(traced_promote(store, head) == 3 && tail_key() == 8, "The trigger did not withhold the tail key update");
    require(traced_promote(store, head) == 3 && tail_key() == 2, "A withheld tail key incorrectly proved canonical ordering");
    require(traced_promote(store, head) == 0, "The repaired integer order did not enable repeated promotion");
}

void exercise(const std::filesystem::path& root) {
    domain::Entry head;
    domain::Id other, keeper;
    Rows settled;
    {
        bool reject = false;
        HistoryStore store(root, [&](std::string_view point) {
            if (reject && point == "transaction") throw std::runtime_error("Injected pre-commit failure");
        });
        auto* db = store.database().handle();
        store.set_capacity("main", 4);
        domain::Format text;
        text.standard_id = 13;
        text.bytes = {'A',0,0,0};
        const auto tail = store.admit("main", {{text}});
        for (int i = 0; i != 3; ++i) store.copy(tail, "main");
        head = store.hanging("main").front();
        other = store.create_rope("Other", 1);
        keeper = store.copy(head.id, other);
        const auto neighbor = keys(store, other);
        Statement anchor(db, "UPDATE rope_view_state SET anchor_entry_id=?,offset=1.25 WHERE rope_id='main'");
        anchor.bind(1, tail);
        anchor.run();
        const auto [version, original_anchor, offset] = viewport(db);
        unsigned follows = 0;
        store.on_head_change = [&](const auto&) { ++follows; };
        const auto initial = keys(store, "main");
        set_key(db, initial[1].first, 7);
        set_key(db, initial[2].first, 7);
        set_key(db, tail, 99);
        const auto legacy = keys(store, "main");
        require(traced_promote(store, head) == 4 && keys(store, "main") == normalized(legacy),
                "First head promotion did not normalize tied and sparse raw keys");
        require(traced_promote(store, head) == 0 && viewport(db) == std::tuple{version + 2, original_anchor, offset},
                "Repeated canonical promotion rewrote entries or lost view state");
        auto stale = head;
        ++stale.revision;
        history_fixture::rejects([&] { store.promote(head.id, &stale); }, "Fast path accepted stale binding");
        require(viewport(db) == std::tuple{version + 2, original_anchor, offset}, "Stale press changed the viewport");

        auto local = keys(store, "main");
        set_key(db, local[1].first, 2); // Same head, tail and row count. Only an interior key changed.
        auto tied = keys(store, "main");
        reject = true;
        history_fixture::rejects([&] { store.promote(head.id, &head); }, "Failed normalization committed");
        reject = false;
        require(keys(store, "main") == tied && viewport(db) == std::tuple{version + 2, original_anchor, offset},
                "Rollback changed raw keys or viewport state");
        require(traced_promote(store, head) == 4 && keys(store, "main") == normalized(tied),
                "Rolled-back witness skipped normalization on retry");
        local = keys(store, "main");
        set_key(db, local[1].first, 2);
        tied = keys(store, "main");
        require(traced_promote(store, head) == 4 && keys(store, "main") == normalized(tied),
                "Same-connection SQL kept a stale witness");

        // Witness invalidation reads the actual pager version even on a profile
        // where ordinary tables expose plausible but constant pragma_* values.
        exec(db, "CREATE TABLE pragma_data_version(data_version INTEGER);"
                 "INSERT INTO pragma_data_version VALUES(777);"
                 "CREATE TEMP TABLE pragma_schema_version(schema_version INTEGER);"
                 "INSERT INTO pragma_schema_version VALUES(999)");
        require(traced_promote(store, head) == 4, "Shadow fixture did not precede a fresh witness");
        auto sibling = history_fixture::observe(root, SQLITE_OPEN_READWRITE);
        local = keys(store, "main");
        set_key(sibling.get(), local[1].first, 2);
        tied = keys(store, "main");
        require(traced_promote(store, head) == 4 && keys(store, "main") == normalized(tied),
                "Sibling WAL commit kept a stale witness");
        // An ignored per-row write can be successful as a SQL statement without normalizing the rope.
        exec(db, "CREATE TEMP TRIGGER skip_normalize BEFORE UPDATE OF sort_key ON entries "
                 "WHEN OLD.sort_key=8 BEGIN SELECT RAISE(IGNORE); END");
        set_key(db, local[1].first, 8);
        require(traced_promote(store, head) == 4 && traced_promote(store, head) == 4,
                "Skipped normalization issued a false witness");
        exec(db, "DROP TRIGGER skip_normalize");
        exec(db, "UPDATE rope_view_state SET anchor_entry_id=(SELECT id FROM entries WHERE rope_id<>'main') WHERE rope_id='main'");
        require(traced_promote(store, head) == 4 && std::get<1>(viewport(db)).empty(), "Invalid anchor survived promotion");
        require(store.entry(head.id).snapshot_id == head.snapshot_id && follows == 0 && keys(store, other) == neighbor,
                "Promotion affected snapshot identity, head-follow callbacks or another rope");
        store.reorder(head.id, 3, &head);
        head = store.hanging("main").front();
        require(follows == 1 && traced_promote(store, head) == 0 && follows == 1,
                "Full reorder did not supply the next canonical-head proof");
        exec(db, "CREATE TEMP TRIGGER side_effect AFTER UPDATE OF version ON rope_view_state "
                 "WHEN NEW.rope_id='main' BEGIN UPDATE entries SET sort_key=8 WHERE rope_id='main' AND sort_key=2; END");
        store.promote(head.id, &head);
        exec(db, "DROP TRIGGER side_effect");
        const auto disrupted = keys(store, "main");
        require(traced_promote(store, head) == 4 && keys(store, "main") == normalized(disrupted),
                "Unexpected trigger writes escaped normalization");
        settled = keys(store, "main");
    }
    HistoryStore reopened(root);
    require(traced_promote(reopened, head) == 4 && keys(reopened, "main") == settled && traced_promote(reopened, head) == 0,
            "Reopen trusted an earlier connection or lost canonical keys");
    domain::Format text;
    text.standard_id = 13;
    text.bytes = {'B',0,0,0};
    const auto admitted = reopened.admit("main", {{text}});
    const auto final = keys(reopened, "main");
    require(final.size() == settled.size() && final.front().first == admitted && final.back().first == settled[2].first &&
            reopened.entry(keeper).rope_id == other, "Next admission picked the wrong victim");
    history_fixture::rejects([&] { reopened.entry(settled.back().first); }, "Evicted tail survived");
}
}
int main() {
    try {
        history_fixture::Directory dir;
        exercise(dir.path / "history");
        retry_ignored_key(dir.path / "retry");
        reorder_with_trigger(dir.path / "middle");
        std::cout << "Canonical promotion, raw keys, external invalidation, rollback, anchors and reopen verified\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
