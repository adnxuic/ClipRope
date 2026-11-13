#include "persistence/history_store.hpp"
#include "persistence/search_fields.hpp"
#include "platform_win/file_group_paths.hpp"
#include <objidl.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cliprope::persistence {
namespace {
using domain::Id;
constexpr char ordered_ids[] =
    "SELECT id FROM entries WHERE state='Hanging' AND rope_id=? ORDER BY sort_key,id";

void run_id(sqlite3* db, const char* sql, std::string_view id) {
    Statement query(db, sql);
    query.bind(1, id);
    query.run();
}

Id new_id(sqlite3* db) {
    Statement query(db, "SELECT lower(hex(randomblob(16)))");
    if (!query.step()) throw std::runtime_error("Could not allocate a stable ID");
    return query.text(0);
}

std::optional<int> rope_capacity(sqlite3* db, std::string_view id) {
    Statement query(db, "SELECT capacity FROM ropes WHERE id=?");
    query.bind(1, id);
    if (!query.step()) throw application::MissingHistoryTarget("Target rope no longer exists");
    if (query.is_null(0)) return {};
    const auto value = query.integer(0);
    if (value < 1 || value > std::numeric_limits<int>::max())
        throw std::runtime_error("Stored rope capacity is out of range");
    return static_cast<int>(value);
}

using RawKeys = std::unordered_map<Id, std::int64_t>;

domain::RopeOrder read_order(sqlite3* db, std::string_view rope, RawKeys* keys = nullptr) {
    Statement query(db, keys ? "SELECT id,CASE WHEN typeof(sort_key)='integer' THEN sort_key END "
                               "FROM entries WHERE state='Hanging' AND rope_id=? ORDER BY sort_key,id" : ordered_ids);
    query.bind(1, rope);
    std::vector<Id> ids;
    while (query.step()) {
        ids.push_back(query.text(0));
        if (keys && !query.is_null(1)) keys->emplace(ids.back(), query.integer(1));
    }
    return domain::RopeOrder(std::move(ids));
}

void changed(sqlite3* db, std::string_view rope) {
    // A moved or detached anchor can still exist, but no longer belongs here.
    run_id(db, "UPDATE rope_view_state SET version=version+1,anchor_entry_id=("
               "SELECT e.id FROM entries e WHERE e.id=anchor_entry_id AND e.rope_id=?1)"
               " WHERE rope_id=?1", rope);
}

bool write_keys(sqlite3* db, const domain::RopeOrder& order, const RawKeys* observed = nullptr) {
    Statement set(db, "UPDATE entries SET sort_key=?1 WHERE id=?2 AND sort_key<>?1");
    bool all_written = true;
    for (std::size_t i = 0; i < order.ids().size(); ++i) {
        const auto& id = order.ids()[i];
        const auto key = static_cast<std::int64_t>(i);
        if (observed) {
            const auto found = observed->find(id);
            if (found != observed->end() && found->second == key) {
                all_written = false;
                continue;
            }
        }
        const auto before = sqlite3_total_changes64(db);
        set.bind(1, key);
        set.bind(2, id);
        set.run();
        const auto direct = sqlite3_changes64(db);
        all_written = all_written && direct == 1;
        // A trigger can rewrite a later rank during this loop. Check subsequent rows in SQL.
        if (sqlite3_total_changes64(db) - before != direct) observed = nullptr;
        set.reset();
    }
    return all_written;
}

std::int64_t head_key(sqlite3* db, std::string_view rope) {
    Statement head(db, "SELECT sort_key FROM entries WHERE state='Hanging' AND rope_id=?"
                       " ORDER BY sort_key,id LIMIT 1");
    head.bind(1, rope);
    if (!head.step()) return 0;
    const auto key = head.integer(0);
    if (key != std::numeric_limits<std::int64_t>::min()) return key - 1;
    write_keys(db, read_order(db, rope));
    return -1;
}

void trim(sqlite3* db, std::string_view rope, std::optional<int> capacity, const Id& inserted = {}, std::size_t position = 0) {
    if (!capacity && !position) {
        changed(db, rope);
        return;
    }
    RawKeys keys;
    auto order = read_order(db, rope, position ? &keys : nullptr);
    if (position) {
        if (capacity && position >= static_cast<std::size_t>(*capacity))
            throw std::out_of_range("The drop slot exceeds the rope capacity");
        order.reorder(inserted, position);
        write_keys(db, order, &keys);
    }
    if (capacity)
        for (const auto& id : order.trim(*capacity)) run_id(db, "DELETE FROM entries WHERE id=?", id);
    changed(db, rope);
}

void insert_entry(sqlite3* db, std::string_view id, std::string_view snapshot, std::string_view rope) {
    Statement add(db, "INSERT INTO entries(id,snapshot_id,state,rope_id,sort_key)"
                      " VALUES(?,?,'Hanging',?,?)");
    add.bind(1, id);
    add.bind(2, snapshot);
    add.bind(3, rope);
    add.bind(4, head_key(db, rope));
    add.run();
}

domain::Entry read_entry(const Statement& row) {
    domain::Entry result;
    result.id = row.text(0);
    result.snapshot_id = row.text(1);
    if (!row.is_null(2)) result.rope_id = row.text(2);
    if (!row.is_null(3)) result.sort_key = row.integer(3);
    result.revision = row.integer(4);
    return result;
}

std::vector<domain::Entry> rows(Statement& query) {
    std::vector<domain::Entry> entries;
    while (query.step()) entries.push_back(read_entry(query));
    return entries;
}

void verify_binding(const domain::Entry& current, const domain::Entry* expected) {
    // Reordering within the same rope does not change the gesture's target.
    if (expected && (current.id != expected->id || current.snapshot_id != expected->snapshot_id ||
                     current.rope_id != expected->rope_id || current.revision != expected->revision))
        throw application::HistoryFailure(application::HistoryIssue::stale_binding, "Bound card content or membership changed");
}

} // namespace

domain::Id HistoryStore::prepare_note_id() {
    std::lock_guard lock(db_.mutex());
    return new_id(db_.handle());
}

HistoryStore::HistoryStore(const std::filesystem::path& root, Database::Fault fault)
    : db_(root, std::move(fault)), content_(db_) {
    // The durable collector is always the most recently selected ordinary rope.
    // A transaction can overlay it with ReBin for this writer session only.
    exec(db_.handle(), "CREATE TEMP TABLE rebin_session(id INTEGER PRIMARY KEY CHECK(id=1), collecting INTEGER NOT NULL);"
                       "CREATE TEMP VIEW navigable_ropes AS SELECT * FROM main.ropes "
                       "WHERE id<>'system:rebin' OR EXISTS(SELECT 1 FROM rebin_session)");
    startup_collection_ = gc();
}

CollectionResult HistoryStore::gc() {
    std::lock_guard guard(admission_files_);
    return content_.collect();
}

void HistoryStore::mutate(const std::function<void()>& change, bool yield_while_busy) {
    std::unique_lock lock(db_.mutex());
    Transaction tx(db_.handle(), yield_while_busy ? &lock : nullptr);
    change();
    db_.fault("transaction");
    tx.commit();
    db_.fault("committed");
}

domain::Id HistoryStore::active_rope() {
    std::lock_guard lock(db_.mutex());
    Statement q(db_.handle(), "SELECT CASE WHEN (SELECT collecting FROM rebin_session) THEN 'system:rebin' "
                              "ELSE active_rope_id END FROM app_settings WHERE id=1");
    if (!q.step()) throw std::runtime_error("Active collector setting is missing");
    return q.text(0);
}

application::CollectorHead HistoryStore::collector_head() {
    std::lock_guard lock(db_.mutex());
    application::CollectorHead head{active_rope()};
    Statement row(db_.handle(), "SELECT id,snapshot_id FROM entries WHERE state='Hanging' AND rope_id=? "
                                "ORDER BY sort_key,id LIMIT 1");
    row.bind(1, head.rope);
    if (row.step()) {
        head.entry = row.text(0);
        head.snapshot = row.text(1);
    }
    return head;
}

void HistoryStore::mutate_head(const std::function<void()>& change, bool yield_while_busy) {
    if (!on_head_change) return mutate(change, yield_while_busy);
    application::HeadChange heads;
    mutate([&] {
        heads.before = collector_head();
        change();
        heads.after = collector_head();
        if (heads.before != heads.after) heads.serial = ++head_serial_;
    }, yield_while_busy);
    if (heads.before != heads.after) {
        try {
            on_head_change(heads);
        } catch (...) {
            // The membership and eviction have committed. Never signal a retry
            // of that transaction because a downstream notification failed.
            ++failed_head_notifications_;
        }
    }
}

domain::Id HistoryStore::create_rope(std::string_view name, int capacity) {
    if (capacity < 1) throw std::invalid_argument("A rope needs a positive capacity");
    Id id;
    mutate([&] { id = insert_rope(name, capacity, {}); });
    return id;
}

void HistoryStore::select_collector(std::string_view id) {
    if (id == domain::rebin_id && !rebin_visible()) throw std::invalid_argument("ReBin is hidden");
    run_id(db_.handle(), "UPDATE rebin_session SET collecting=(?='system:rebin')", id);
    if (id != domain::rebin_id)
        run_id(db_.handle(), "UPDATE app_settings SET active_rope_id=? WHERE id=1", id);
}

void HistoryStore::set_active_rope(std::string_view id, bool hide_rebin) {
    mutate_head([&] {
        rope_capacity(db_.handle(), id);
        if (hide_rebin) exec(db_.handle(), "DELETE FROM rebin_session");
        select_collector(id);
    });
}

domain::Entry HistoryStore::entry(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    Statement q(db_.handle(), "SELECT id,snapshot_id,rope_id,sort_key,revision FROM entries WHERE id=?");
    q.bind(1, id);
    if (!q.step()) throw application::MissingHistoryTarget("Entry no longer exists");
    return read_entry(q);
}

std::vector<domain::Entry> HistoryStore::hanging(std::string_view rope) {
    std::lock_guard lock(db_.mutex());
    rope_capacity(db_.handle(), rope);
    Statement q(db_.handle(), "SELECT id,snapshot_id,rope_id,sort_key,revision FROM entries"
                              " WHERE state='Hanging' AND rope_id=? ORDER BY sort_key,id");
    q.bind(1, rope);
    return rows(q);
}

std::vector<domain::Entry> HistoryStore::floating() {
    std::lock_guard lock(db_.mutex());
    Statement q(db_.handle(), "SELECT id,snapshot_id,rope_id,sort_key,revision FROM entries"
                              " WHERE state='Floating' ORDER BY id");
    return rows(q);
}

domain::Layout HistoryStore::layout(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    Statement q(db_.handle(), "SELECT monitor,x,y,width,height,visible FROM card_presentation WHERE entry_id=?");
    q.bind(1, id);
    if (!q.step()) throw std::out_of_range("Card layout is missing");
    return {q.text(0), q.real(1), q.real(2), q.real(3), q.real(4), q.integer(5) != 0};
}

void HistoryStore::reveal_floating(const domain::Entry& expected, const domain::Layout& saved, const domain::Layout& placed) {
    if (expected.rope_id || saved.visible || !placed.visible) throw std::invalid_argument("Invalid Floating reveal");
    mutate([&] {
        verify_binding(entry(expected.id), &expected);
        if (layout(expected.id) != saved)
            throw application::HistoryFailure(application::HistoryIssue::stale_binding, "Floating placement changed");
        write_floating_layout(expected.id, placed);
    });
}

domain::LeasedSnapshot HistoryStore::load(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    return content_.load(entry(id).snapshot_id);
}

domain::LeasedSnapshot HistoryStore::load_snapshot(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    return content_.load(id);
}

bool HistoryStore::has_external_payload(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT 1 FROM representations r JOIN blobs b ON b.id=r.blob_id "
                                "WHERE r.snapshot_id=? AND b.relative_path IS NOT NULL LIMIT 1");
    row.bind(1, id);
    return row.step();
}

application::PreparedSnapshot HistoryStore::read_for_display(std::string_view id, std::stop_token stop) {
    // A separate WAL reader lets the UI query summaries while a file read is pending.
    Database reader(db_, Database::SnapshotConnection{});
    return {domain::Id(id), ContentRepository(reader).load(id, stop)};
}

std::filesystem::path HistoryStore::materialize_screenshot(const domain::Entry& entry,
                                                          std::span<const unsigned char> png) {
    std::lock_guard files(admission_files_);
    Database writer(db_, Database::AdmissionConnection{});
    return managed_screenshot(writer, entry, png);
}

std::string HistoryStore::snip_original(const Id& snapshot, std::string_view candidate,
                                        std::optional<std::pair<std::int64_t, std::int64_t>> window, bool remember) {
    if (candidate.empty()) return {};
    {
        std::unique_lock lock(db_.mutex());
        auto* db = db_.handle();
        std::optional<Transaction> tx;
        if (remember) {
            db_.fault("binding-original");
            tx.emplace(db, &lock); // A concurrent WAL writer must not block unrelated reads on the primary connection.
        }
        Statement live(db, "SELECT screenshot_path FROM snapshots WHERE id=? AND "
                           "EXISTS(SELECT 1 FROM entries WHERE snapshot_id=snapshots.id)");
        live.bind(1, snapshot);
        if (!live.step()) return {};
        if (auto installed = live.text(0); !installed.empty()) return installed;
        if (window) {
            Statement other(db, "SELECT 1 FROM representations own JOIN representations r ON r.blob_id=own.blob_id "
                                "JOIN snapshots s ON s.id=r.snapshot_id WHERE own.snapshot_id=?1 "
                                "AND own.format_name='PNG' COLLATE NOCASE AND s.id<>?1 AND s.source_hint="
                                "(SELECT source_hint FROM snapshots WHERE id=?1) COLLATE NOCASE AND s.captured_at BETWEEN ?2 AND ?3 LIMIT 1");
            other.bind(1, snapshot);
            other.bind(2, window->first);
            other.bind(3, window->second);
            if (other.step()) return {};
        }
        if (!remember) return std::string(candidate);
        Statement bind(db, "UPDATE snapshots SET screenshot_path=? WHERE id=?");
        bind.bind(1, candidate);
        bind.bind(2, snapshot);
        bind.run();
        insert_origin_field(db, snapshot, candidate);
        tx->commit();
    }
    if (on_snip_bound) on_snip_bound();
    return std::string(candidate);
}

namespace {
Id admit_snapshot(Database& store, ContentRepository& content, std::string_view rope, const PreparedContent& prepared) {
    Id id;
    auto* db = store.handle();
    Transaction tx(db);
    const auto capacity = rope_capacity(db, rope);
    store.fault("admission-write-start");
    const auto saved = content.install(prepared);
    id = new_id(db);
    insert_entry(db, id, saved, rope);
    trim(db, rope, capacity);
    store.fault("transaction");
    tx.commit();
    store.fault("committed");
    return id;
}
}
domain::Id HistoryStore::admit(std::string_view rope, const domain::Snapshot& snapshot) {
    PreparedContent prepared(snapshot, [this](auto stage) { db_.fault(stage); });
    std::lock_guard lock(db_.mutex());
    return admit_snapshot(db_, content_, rope, prepared);
}
domain::Id HistoryStore::admit_capture(std::string_view rope, const domain::Snapshot& snapshot) {
    PreparedContent prepared(snapshot, [this](auto stage) { db_.fault(stage); });
    std::lock_guard files(admission_files_);
    Database writer(db_, Database::AdmissionConnection{});
    ContentRepository content(writer);
    return admit_snapshot(writer, content, rope, prepared);
}

void HistoryStore::reorder(std::string_view id, std::size_t position, const domain::Entry* expected) {
    std::optional<NormalizedHead> result;
    mutate_head([&] { result = reorder_in_transaction(id, position, expected, false); });
    remember_normalization(std::move(result));
}

std::optional<HistoryStore::NormalizedHead> HistoryStore::reorder_in_transaction(
        std::string_view id, std::size_t position, const domain::Entry* expected, bool promotion) {
    auto previous = std::exchange(normalized_, {}); // A failed attempt cannot publish a proof of uncommitted ordering.
    const auto item = entry(id);
    verify_binding(item, expected);
    if (!item.rope_id) throw std::invalid_argument("Floating cards have no rope order");
    auto* db = db_.handle();
    // Ordinary tables can shadow the eponymous pragma_* virtual tables.
    const auto version = [db](const char* sql) {
        Statement row(db, sql);
        if (!row.step()) throw std::runtime_error("Cannot read history version");
        return row.integer(0);
    };
    const auto external = version("PRAGMA main.data_version");
    const auto schema = version("PRAGMA main.schema_version");
    const auto local = sqlite3_total_changes64(db);
    if (promotion && previous && previous->head == id && previous->rope == *item.rope_id &&
        item.sort_key == 0 && previous->data_version == external && previous->schema_version == schema && previous->changes == local) {
        changed(db, *item.rope_id);
        const auto after = sqlite3_total_changes64(db);
        // If a trigger modified other rows, the canonical order needs checking on the next recall.
        if (after - local != sqlite3_changes64(db)) return {};
        previous->changes = after;
        return previous;
    }
    RawKeys keys;
    const auto observed = promotion ? nullptr : &keys;
    auto order = read_order(db, *item.rope_id, observed);
    order.reorder(item.id, position);
    const auto all_written = write_keys(db, order, observed);
    changed(db, *item.rope_id);
    const auto after = sqlite3_total_changes64(db);
    if (!all_written || after - local != static_cast<std::int64_t>(order.ids().size()) + sqlite3_changes64(db)) {
        // Some conditional writes were skipped, or a trigger modified other rows. Prove the
        // exact raw keys before trusting this normalization. A full rotation needs no extra scan.
        Statement keys(db, "SELECT sort_key FROM entries WHERE rope_id=? AND state='Hanging' ORDER BY sort_key,id");
        keys.bind(1, *item.rope_id);
        std::int64_t next{};
        while (keys.step())
            if (keys.integer(0) != next++) return {};
    }
    return NormalizedHead{*item.rope_id, order.ids().front(), external, schema, after};
}

void HistoryStore::remember_normalization(std::optional<NormalizedHead> result) {
    // Called after successful COMMIT. Intervening writes (including those in a head callback)
    // invalidate the sampled connection counters before the next use.
    std::lock_guard lock(db_.mutex());
    normalized_ = std::move(result);
}

void HistoryStore::promote(std::string_view id, const domain::Entry* expected) {
    // Explicit recall already published this entry before promoting it.
    std::optional<NormalizedHead> result;
    mutate([&] { result = reorder_in_transaction(id, 0, expected, true); });
    remember_normalization(std::move(result));
}

void HistoryStore::resize(const domain::Entry& expected, domain::CardScale scale, const domain::Layout* placement) {
    const auto valid = [](float value) { return value >= domain::CardScale::minimum && value <= domain::CardScale::maximum; };
    if (!valid(scale.width) || !valid(scale.height) || expected.rope_id.has_value() == bool(placement))
        throw std::invalid_argument("Invalid card resize");
    mutate([&] {
        verify_binding(entry(expected.id), &expected);
        Statement update(db_.handle(), "UPDATE card_presentation SET width_scale=?,height_scale=? WHERE entry_id=?");
        update.bind(1, double(scale.width));
        update.bind(2, double(scale.height));
        update.bind(3, expected.id);
        update.run();
        if (placement) write_floating_layout(expected.id, *placement);
    });
}

void HistoryStore::detach(std::string_view id, const domain::Layout& layout, const domain::Entry* expected) {
    mutate_head([&] {
        auto item = entry(id);
        verify_binding(item, expected);
        const auto previous_rope = item.detach();
        auto* db = db_.handle();
        run_id(db, "UPDATE entries SET state='Floating',rope_id=NULL,sort_key=NULL WHERE id=?", id);
        write_floating_layout(id, layout);
        changed(db, previous_rope);
    });
}

void HistoryStore::transfer(std::string_view id, std::string_view target, bool require_floating,
                            const domain::Entry* expected, std::size_t position) {
    mutate_head([&] {
        auto* db = db_.handle();
        const auto capacity = rope_capacity(db, target);
        const auto item = entry(id);
        verify_binding(item, expected);
        if (require_floating == item.rope_id.has_value())
            throw std::invalid_argument("The card's membership has changed");
        Statement move(db, "UPDATE entries SET state='Hanging',rope_id=?,sort_key=? WHERE id=?");
        move.bind(1, target);
        move.bind(2, head_key(db, target));
        move.bind(3, id);
        move.run();
        if (item.rope_id && *item.rope_id != target) changed(db, *item.rope_id);
        trim(db, target, capacity, item.id, position);
    });
}

void HistoryStore::rehang(std::string_view id, std::string_view target, const domain::Entry* expected) {
    rehang_at(id, target, 0, expected);
}
void HistoryStore::rehang_at(std::string_view id, std::string_view target, std::size_t position,
                             const domain::Entry* expected) {
    transfer(id, target, true, expected, position);
}
void HistoryStore::move(std::string_view id, std::string_view target, const domain::Entry* expected, std::size_t position) {
    transfer(id, target, false, expected, position);
}

domain::Id HistoryStore::copy(std::string_view id, std::string_view target, const domain::Entry* expected, std::size_t position) {
    Id copy_id;
    mutate_head([&] {
        auto* db = db_.handle();
        const auto capacity = rope_capacity(db, target);
        const auto original = entry(id);
        verify_binding(original, expected);
        copy_id = new_id(db);
        insert_entry(db, copy_id, original.snapshot_id, target);
        Statement style(db, "UPDATE card_presentation SET (font_size,font_override,bold,text_ink,paper_tint,opacity,width_scale,height_scale)="
                        "(SELECT font_size,font_override,bold,text_ink,paper_tint,opacity,width_scale,height_scale FROM card_presentation WHERE entry_id=?)"
                        " WHERE entry_id=?");
        style.bind(1, id);
        style.bind(2, copy_id);
        style.run();
        trim(db, target, capacity, copy_id, position);
    });
    return copy_id;
}

void HistoryStore::erase(std::string_view id, const domain::Entry* expected) {
    mutate_head([&] {
        const auto item = entry(id);
        verify_binding(item, expected);
        run_id(db_.handle(), "DELETE FROM entries WHERE id=?", id);
        if (item.rope_id) changed(db_.handle(), *item.rope_id);
    });
}

application::ExpandFileGroup HistoryStore::prepare_expansion(const domain::Entry& expected, bool keep_source) {
    std::lock_guard lock(db_.mutex());
    verify_binding(entry(expected.id), &expected);
    auto saved = content_.load(expected.snapshot_id);
    const auto& formats = saved.snapshot.formats;
    const auto drop = std::find_if(formats.begin(), formats.end(), [](const domain::Format& f) {
        return f.standard_id == CF_HDROP && f.outcome == "saved" && f.codec_version == 1 &&
               f.tymed == TYMED_HGLOBAL && f.aspect == DVASPECT_CONTENT && f.index == -1;
    });
    if (drop == formats.end()) throw std::invalid_argument("No saved file references");
    auto paths = file_group_paths(drop->bytes);
    if (paths.size() < 2) throw std::invalid_argument("Expansion requires multiple saved paths");
    application::ExpandFileGroup group{expected, saved.snapshot.source_hint, saved.snapshot.captured_at};
    group.keep_source = keep_source;
    for (auto& path : paths)
        group.members.push_back({new_id(db_.handle()), std::move(path)});
    return group;
}

namespace {
application::ExpansionImpact expansion_impact(const std::vector<Id>& order, std::size_t after_source,
                                              std::size_t count, std::optional<int> capacity) {
    if (!capacity) return {};
    const auto limit = static_cast<std::size_t>(*capacity);
    if (after_source + count > limit) return {after_source + count, {}};
    if (order.size() + count <= limit) return {};
    return {0, {order.begin() + (limit - count), order.end()}};
}

std::size_t expansion_position(std::vector<Id>& order, const application::ExpandFileGroup& group) {
    const auto source = std::find(order.begin(), order.end(), group.source.id);
    if (source == order.end()) throw std::runtime_error("Source card no longer belongs to this rope");
    const auto index = static_cast<std::size_t>(source - order.begin());
    if (group.keep_source) return index + 1;
    order.erase(source);
    return index;
}
} // namespace

application::ExpansionImpact HistoryStore::preview_expansion(const application::ExpandFileGroup& group) {
    std::lock_guard lock(db_.mutex());
    verify_binding(entry(group.source.id), &group.source);
    if (!group.source.rope_id) return {};
    const auto& rope = *group.source.rope_id;
    auto order = read_order(db_.handle(), rope).ids();
    const auto position = expansion_position(order, group);
    return expansion_impact(order, position,
                            group.members.size(), rope_capacity(db_.handle(), rope));
}

bool HistoryStore::expand_file_group(const application::ExpandFileGroup& group,
                                     const application::ExpansionImpact& approved,
                                     const std::vector<domain::Layout>& layouts) {
    const auto& rope = group.source.rope_id;
    if (group.members.size() < 2 || layouts.size() != (rope ? 0 : group.members.size()) || approved.required_capacity)
        throw std::invalid_argument("Invalid file group expansion");
    bool applied = false;
    mutate_head([&] {
        auto* db = db_.handle();
        verify_binding(entry(group.source.id), &group.source);
        std::vector<Id> order;
        std::size_t position{};
        if (rope) {
            order = read_order(db, *rope).ids();
            position = expansion_position(order, group);
            if (expansion_impact(order, position, group.members.size(), rope_capacity(db, *rope)) != approved)
                return;
            for (const auto& id : approved.removed_tail) run_id(db, "DELETE FROM entries WHERE id=?", id);
            order.resize(order.size() - approved.removed_tail.size());
            order.insert(order.begin() + position, group.members.size(), Id{});
        }
        if (!group.keep_source) run_id(db, "DELETE FROM entries WHERE id=?", group.source.id);
        domain::Format effect;
        effect.name = "Preferred DropEffect";
        effect.bytes = copy_effect_bytes();
        effect.priority = 1;
        for (std::size_t i = 0; i < group.members.size(); ++i) {
            const auto& member = group.members[i];
            domain::Format drop;
            drop.standard_id = CF_HDROP;
            drop.bytes = encode_file_group({member.path});
            domain::Snapshot single{{std::move(drop), effect}, group.source_hint, "captured", group.captured_at};
            single.path_kind = member.kind;
            const auto snapshot = content_.install(single);
            if (rope) {
                insert_entry(db, member.id, snapshot, *rope);
                order[position + i] = member.id;
            } else {
                Statement add(db, "INSERT INTO entries(id,snapshot_id,state) VALUES(?,?,'Floating')");
                add.bind(1, member.id);
                add.bind(2, snapshot);
                add.run();
                write_floating_layout(member.id, layouts[i]);
            }
        }
        if (rope) {
            write_keys(db, domain::RopeOrder(std::move(order)));
            changed(db, *rope);
        }
        applied = true;
    });
    return applied;
}
} // namespace cliprope::persistence
