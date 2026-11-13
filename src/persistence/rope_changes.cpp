#include "persistence/history_store.hpp"
#include "persistence/rope_read.hpp"
#include "persistence/color_store.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cliprope::persistence {
namespace {
using application::RopeChange;
using application::RopeOp;
using application::RopePreview;

void run(sqlite3 *db, const char *sql, std::string_view id) {
    Statement command(db, sql);
    command.bind(1, id);
    command.run();
}
void append(std::vector<unsigned char> &bytes, std::string_view field) {
    const auto length = std::to_string(field.size());
    bytes.insert(bytes.end(), length.begin(), length.end());
    bytes.push_back(':');
    bytes.insert(bytes.end(), field.begin(), field.end());
}
void metadata(std::vector<unsigned char> &stamp, const domain::Rope &rope) {
    append(stamp, rope.id);
    append(stamp, rope.name);
    append(stamp, rope.color.encode());
    append(stamp, std::to_string(rope.capacity.value_or(0)));
}
size_t collect(sqlite3 *db, std::vector<unsigned char> &stamp, std::string_view rope) {
    Statement ids(db, "SELECT id FROM entries WHERE rope_id=? AND state='Hanging' ORDER BY sort_key,id");
    ids.bind(1, rope);
    size_t n = 0;
    while (ids.step()) {
        append(stamp, ids.text(0));
        ++n;
    }
    return n;
}
void trim(sqlite3 *db, std::string_view rope, int capacity) {
    Statement remove(db, "DELETE FROM entries WHERE id IN (SELECT id FROM entries WHERE state='Hanging' AND "
                         "rope_id=? ORDER BY sort_key,id LIMIT -1 OFFSET ?)");
    remove.bind(1, rope);
    remove.bind(2, capacity);
    remove.run();
}
void refresh_view(sqlite3 *db, std::string_view id) {
    run(db, "UPDATE rope_view_state SET version=version+1 WHERE rope_id=?", id);
}
std::vector<domain::Id> navigable_order(sqlite3* db) {
    Statement rows(db, "SELECT id FROM navigable_ropes ORDER BY sort_rank,id");
    std::vector<domain::Id> ids;
    while (rows.step()) ids.push_back(rows.text(0));
    return ids;
}
void save_ranks(sqlite3* db, const std::vector<domain::Id>& ids) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
        Statement rank(db, "UPDATE ropes SET sort_rank=?1 WHERE id=?2 AND sort_rank<>?1");
        rank.bind(1, static_cast<std::int64_t>(i));
        rank.bind(2, ids[i]);
        rank.run();
    }
}
} // namespace

void require_rope_name(std::string_view value) {
    constexpr int limit = application::rope_name_units;
    if (value.empty() || value.size() > 3 * limit || value.find('\0') != value.npos)
        throw application::InvalidRopeName{};
    wchar_t decoded[limit]{};
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), decoded, limit);
    WORD types[limit]{};
    if (!units || !GetStringTypeW(CT_CTYPE1, decoded, units, types) ||
        std::none_of(types, types + units, [](WORD type) { return !(type & C1_SPACE); }))
        throw application::InvalidRopeName{};
}

double HistoryStore::rope_top(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT top_dip FROM rope_view_state WHERE rope_id=?");
    row.bind(1, id);
    if (!row.step()) throw std::out_of_range("Rope unavailable");
    return row.real(0);
}
void HistoryStore::save_rope_top(std::string_view id, double dip) {
    if (!std::isfinite(dip) || dip < 0) throw std::invalid_argument("Invalid rope height");
    mutate([&] {
        Statement row(db_.handle(), "UPDATE rope_view_state SET top_dip=? WHERE rope_id=?");
        row.bind(1, dip);
        row.bind(2, id);
        row.run();
        if (sqlite3_changes(db_.handle()) != 1) throw std::out_of_range("Rope unavailable");
    });
}
bool HistoryStore::rebin_visible() {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT 1 FROM rebin_session");
    return row.step();
}
void HistoryStore::set_rebin_visible(bool visible) {
    mutate_head([&] {
        if (visible == rebin_visible()) return;
        if (visible) {
            auto ids = navigable_order(db_.handle());
            const auto current = std::find(ids.begin(), ids.end(), active_rope());
            if (current == ids.end()) throw std::runtime_error("Collector unavailable");
            ids.insert(current, domain::rebin_id);
            save_ranks(db_.handle(), ids);
            exec(db_.handle(), "INSERT INTO rebin_session VALUES(1,1)");
        } else exec(db_.handle(), "DELETE FROM rebin_session");
    });
}
domain::Id HistoryStore::reorder_rope(std::string_view source, std::string_view target, std::string_view expected_head) {
    domain::Id collector;
    mutate_head([&] {
        if (active_rope() != expected_head) throw std::runtime_error("The collector changed during the rope drag");
        auto ids = navigable_order(db_.handle());
        const auto head = std::find(ids.begin(), ids.end(), expected_head);
        if (head == ids.end()) throw std::out_of_range("Collector unavailable");
        std::rotate(ids.begin(), head, ids.end());
        const auto from = std::find(ids.begin(), ids.end(), source);
        const auto to = std::find(ids.begin(), ids.end(), target);
        if (from == ids.end() || to == ids.end()) throw std::out_of_range("Rope unavailable");
        collector = ids.front();
        if (from == to) return;
        if (from < to) std::rotate(from, from + 1, to + 1);
        else std::rotate(to, from, from + 1);
        save_ranks(db_.handle(), ids);
        collector = ids.front();
        select_collector(collector);
    });
    return collector;
}
int HistoryStore::default_capacity() {
    std::lock_guard lock(db_.mutex());
    Statement query(db_.handle(), "SELECT default_capacity FROM app_settings WHERE id=1");
    if (!query.step())
        throw std::runtime_error("Default capacity unavailable");
    return static_cast<int>(query.integer(0));
}
bool HistoryStore::rope_compact(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT compact FROM rope_view_state WHERE rope_id=?");
    row.bind(1, id);
    if (!row.step()) throw std::out_of_range("Rope unavailable");
    return row.integer(0) != 0;
}
void HistoryStore::save_rope_compact(std::string_view id, bool compact, const application::Anchor& anchor) {
    if (!std::isfinite(anchor.offset) || anchor.offset < 0)
        throw std::invalid_argument("Invalid scroll offset");
    mutate([&] {
        Statement row(db_.handle(), "UPDATE rope_view_state SET compact=?, anchor_entry_id=(SELECT id FROM entries "
                                   "WHERE id=? AND state='Hanging' AND rope_id=?), offset=? WHERE rope_id=?");
        row.bind(1, compact ? 1 : 0);
        row.bind(2, anchor.entry);
        row.bind(3, id);
        row.bind(4, anchor.offset);
        row.bind(5, id);
        row.run();
        if (sqlite3_changes(db_.handle()) != 1) throw std::out_of_range("Rope unavailable");
    });
}
domain::Id HistoryStore::insert_rope(std::string_view name, int capacity, domain::ColorSelection color) {
    require_rope_name(name);
    Statement add(db_.handle(),
                  "INSERT INTO ropes(id,name,color_value,capacity,sort_rank) SELECT "
                  "lower(hex(randomblob(16))),?,?,?,coalesce(max(sort_rank),0)+1 FROM ropes RETURNING id");
    add.bind(1, name);
    add.bind(2, color.encode());
    add.bind(3, capacity);
    if (!add.step())
        throw std::runtime_error("Could not create rope");
    auto id = add.text(0);
    add.step();
    write_rope_material(db_.handle(), id, color, domain::RopeMaterial::from(color));
    return id;
}
RopePreview HistoryStore::preview_rope_change(const RopeChange &change) {
    std::lock_guard lock(db_.mutex());
    auto *db = db_.handle();
    RopePreview preview{change};
    std::vector<unsigned char> stamp;
    for (const auto &input : {std::to_string(static_cast<int>(change.op)), change.rope, change.target,
                              change.name, change.color.encode(), std::to_string(change.capacity.value_or(0))})
        append(stamp, input);
    if (change.op != RopeOp::erase && !change.target.empty())
        throw std::invalid_argument("Unexpected transfer target");
    if (change.op == RopeOp::create || change.op == RopeOp::properties) {
        if (change.op == RopeOp::properties && read_rope(db_, change.rope).is_main && change.name != "main")
            throw std::invalid_argument("The main rope must be named main");
        require_rope_name(change.name);
        if (change.color.custom && change.color.custom->a != 255)
            throw std::invalid_argument("Invalid rope color");
    }
    const bool rebin_properties = change.op == RopeOp::properties && change.rope == domain::rebin_id;
    if (rebin_properties && (change.name != "ReBin" || change.capacity))
        throw std::invalid_argument("ReBin has a fixed name and unlimited capacity");
    if ((change.op == RopeOp::create || (change.op == RopeOp::properties && !rebin_properties) || change.op == RopeOp::defaults) &&
        change.capacity < 1)
        throw std::invalid_argument("Capacity must be positive");
    if (change.op == RopeOp::defaults || change.op == RopeOp::create) {
        append(stamp, std::to_string(default_capacity()));
    } else if (change.op == RopeOp::clear_all || change.op == RopeOp::delete_all) {
        Statement all(
            db, "SELECT id,state,rope_id FROM entries WHERE state='Hanging' OR ? ORDER BY rope_id,sort_key,id");
        all.bind(1, change.op == RopeOp::delete_all ? 1 : 0);
        while (all.step()) {
            append(stamp, all.text(0));
            append(stamp, all.text(1));
            append(stamp, all.text(2));
            if (all.text(1) == "Floating")
                ++preview.floating;
            else
                ++preview.hanging;
        }
    } else {
        const auto rope = read_rope(db_, change.rope);
        metadata(stamp, rope);
        const auto count = collect(db, stamp, rope.id);
        if (change.op == RopeOp::properties) {
            preview.shrink = change.capacity < rope.capacity;
            if (change.capacity) preview.hanging = count - std::min(count, static_cast<size_t>(*change.capacity));
        } else if (change.op == RopeOp::clear)
            preview.hanging = count;
        else if (change.op == RopeOp::erase) {
            if (rope.protected_rope())
                throw std::invalid_argument("A system rope cannot be deleted");
            if (change.target.empty())
                preview.hanging = count;
            else {
                if (change.target == change.rope)
                    throw std::invalid_argument("Transfer requires a different rope");
                const auto target = read_rope(db_, change.target);
                metadata(stamp, target);
                const auto combined = count + collect(db, stamp, target.id);
                preview.moved = count;
                preview.overflow = combined - std::min(combined, target.slot_limit());
                preview.hanging = preview.overflow;
            }
        } else
            throw std::invalid_argument("Unknown rope operation");
    }
    preview.stamp = sha256(stamp);
    return preview;
}
std::optional<domain::Id> HistoryStore::apply_rope_change(const RopePreview &consent) {
    std::optional<domain::Id> created;
    mutate_head([&] {
        auto *db = db_.handle();
        const auto &c = consent.change;
        const auto now = preview_rope_change(c);
        if (now.stamp != consent.stamp || now.hanging != consent.hanging || now.floating != consent.floating ||
            now.moved != consent.moved || now.overflow != consent.overflow || now.shrink != consent.shrink)
            throw std::runtime_error("The preview changed; review the latest affected cards");
        if (c.op == RopeOp::create) {
            created = insert_rope(c.name, *c.capacity, c.color);
            remember_color(db, c.color);
        } else if (c.op == RopeOp::defaults) {
            Statement set(db, "UPDATE app_settings SET default_capacity=? WHERE id=1");
            set.bind(1, *c.capacity);
            set.run();
        } else if (c.op == RopeOp::properties) {
            const auto original = read_rope(db_, c.rope);
            Statement set(db, "UPDATE ropes SET name=?,color_value=?,capacity=? WHERE id=?");
            set.bind(1, c.name);
            set.bind(2, c.color.encode());
            if (c.capacity) set.bind(3, *c.capacity);
            else set.bind(3, nullptr);
            set.bind(4, c.rope);
            set.run();
            if (original.color != c.color) {
                write_rope_material(db, c.rope, c.color, domain::RopeMaterial::from(c.color));
                remember_color(db, c.color);
            }
            if (c.capacity) trim(db, c.rope, *c.capacity);
            refresh_view(db, c.rope);
        } else if (c.op == RopeOp::clear) {
            run(db, "DELETE FROM entries WHERE state='Hanging' AND rope_id=?", c.rope);
            refresh_view(db, c.rope);
        } else if (c.op == RopeOp::clear_all || c.op == RopeOp::delete_all) {
            exec(db, c.op == RopeOp::delete_all ? "DELETE FROM entries"
                                                : "DELETE FROM entries WHERE state='Hanging'");
            exec(db, "UPDATE rope_view_state SET version=version+1");
        } else {
            if (!c.target.empty()) {
                Statement move(db, "WITH ranks AS MATERIALIZED (SELECT id,row_number() OVER (ORDER BY CASE "
                                   "rope_id WHEN ?1 THEN 0 ELSE 1 END,sort_key,id)-1 AS k "
                                   "FROM entries WHERE state='Hanging' AND rope_id IN (?1,?2)) "
                                   "UPDATE entries SET rope_id=?2, sort_key=(SELECT k FROM ranks WHERE "
                                   "ranks.id=entries.id) WHERE id IN(SELECT id FROM ranks)");
                move.bind(1, c.rope);
                move.bind(2, c.target);
                move.run();
                if (auto capacity = read_rope(db_, c.target).capacity) trim(db, c.target, *capacity);
                refresh_view(db, c.target);
            } else
                run(db, "DELETE FROM entries WHERE state='Hanging' AND rope_id=?", c.rope);
            run(db,
                "UPDATE app_settings SET active_rope_id=(SELECT id FROM ropes WHERE is_main=1) WHERE "
                "active_rope_id=?",
                c.rope);
            run(db, "DELETE FROM ropes WHERE id=? AND is_main=0", c.rope);
            if (sqlite3_changes(db) != 1)
                throw std::runtime_error("Rope deletion failed");
        }
    });
    return created;
}

void HistoryStore::erase_rope(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    RopeChange change;
    change.op = RopeOp::erase;
    change.rope = id;
    apply_rope_change(preview_rope_change(change));
}
void HistoryStore::set_capacity(std::string_view id, int capacity) {
    std::lock_guard lock(db_.mutex());
    auto rope = read_rope(db_, id);
    RopeChange change;
    change.op = RopeOp::properties;
    change.rope = rope.id;
    change.name = rope.name;
    change.color = rope.color;
    change.capacity = capacity;
    apply_rope_change(preview_rope_change(change));
}
void HistoryStore::save_rope_color(std::string_view id, domain::ColorSelection original,
                                   domain::ColorSelection color) {
    if (color.custom && color.custom->a != 255)
        throw std::invalid_argument("Rope tint must be opaque");
    mutate([&] {
        auto current = read_rope(db_, id);
        if (current.color != original)
            throw std::runtime_error("Rope color changed");
        if (color == original)
            return;
        write_rope_material(db_.handle(), id, color, domain::RopeMaterial::from(color));
        remember_color(db_.handle(), color);
        refresh_view(db_.handle(), id);
    });
}
} // namespace cliprope::persistence
