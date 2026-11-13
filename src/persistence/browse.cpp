#include "persistence/browse.hpp"
#include "persistence/history_store.hpp"
#include "persistence/rope_read.hpp"
#include "platform_win/file_group_paths.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <objidl.h>

namespace cliprope::persistence {
namespace fs = std::filesystem;
namespace {
// Only the visible summaries need a prefix. A large external text payload never
// gets loaded in full to draw a card or populate the tray.
constexpr int prefix_bytes = 2 * application::card_text_prefix_units;
constexpr std::string_view summary_columns =
    "SELECT e.id,e.snapshot_id,e.rope_id,e.sort_key,e.revision,b.id,substr(b.inline_bytes,1,?),b.relative_path,"
    "CASE WHEN c.font_override THEN c.font_size ELSE prefs.text_font_size END,c.bold,c.text_ink,c.paper_tint,c.opacity,s.capture_status,s.path_kind,c.width_scale,c.height_scale,prefs.hover_resolution,prefs.image_file_previews "
    "FROM entries e JOIN snapshots s ON s.id=e.snapshot_id JOIN card_presentation c ON c.entry_id=e.id "
    "JOIN app_settings prefs ON prefs.id=1 LEFT JOIN blobs b ON b.id=("
    "SELECT blob_id FROM representations WHERE snapshot_id=e.snapshot_id AND standard_id=13 "
    "AND status='saved' ORDER BY ordinal LIMIT 1) ";

std::wstring preview(Database& db, Statement& row, bool& unavailable, PageReadStats* stats) {
    if (row.is_null(5)) return {};
    std::vector<unsigned char> prefix;
    if (row.is_null(7)) {
        prefix = row.blob(6);
    } else {
        auto path = safe_blob_path(db.root(), row.text(5));
        std::ifstream stream(path, std::ios::binary);
        if (!stream) { unavailable = true; return {}; }
        prefix.resize(prefix_bytes);
        stream.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
        prefix.resize(static_cast<std::size_t>(stream.gcount()));
    }
    if (stats) stats->prefix_bytes += prefix.size();
    std::wstring units;
    for (std::size_t i = 1; i < prefix.size(); i += 2) {
        units.push_back(static_cast<wchar_t>(prefix[i - 1] | (prefix[i] << 8)));
    }
    return units;
}

StoredCard read_summary(Database& db, Statement& row, PageReadStats* stats = nullptr) {
    StoredCard result;
    result.entry.id = row.text(0);
    result.entry.snapshot_id = row.text(1);
    if (!row.is_null(2)) result.entry.rope_id = row.text(2);
    if (!row.is_null(3)) result.entry.sort_key = row.integer(3);
    result.entry.revision = row.integer(4);
    if (!row.is_null(5)) result.text = preview(db, row, result.text_unavailable, stats);
    result.style.font_size = row.real(8);
    result.style.bold = row.integer(9) != 0;
    result.style.text_color = domain::ColorSelection::decode(row.text(10));
    result.style.background_color = domain::ColorSelection::decode(row.text(11));
    result.style.opacity = row.real(12);
    result.hover_resolution = row.integer(17) != 0;
    result.image_file_previews = row.integer(18) != 0;
    result.scale = {float(row.real(15)), float(row.real(16))};
    result.partial = row.text(13) == "partial";
    result.path_kind = static_cast<domain::PathKind>(row.integer(14));
    return result;
}

domain::Rope rope_from_row(const Statement& row) {
    domain::Rope result{row.text(0), row.text(1), row.is_null(2) ? std::nullopt : std::optional(static_cast<int>(row.integer(2))), row.integer(3) != 0,
                        domain::ColorSelection::decode(row.text(4))};
    require_rope_name(result.name);
    result.material.natural = row.integer(5) != 0;
    for(int i=0;i<5;++i) result.material.tones[i] = unsigned(row.integer(i+6));
    result.material.seed = unsigned(row.integer(11));
    return result;
}

std::wstring wide(const std::string& name) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                      static_cast<int>(name.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n)
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), w.data(), n);
    // A registered name is untrusted display text. Keep each format on its own line.
    for(auto& unit:w)if(unit<L' ' || unit==0x7f)unit=L' ';
    return w;
}
std::string canonical_name(std::string name) {
    const std::string_view known[] = {"HTML Format",          "Rich Text Format", "PNG",
                                      "FileGroupDescriptorW", "FileContents",     "Preferred DropEffect"};
    auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    for (auto candidate : known)
        if (std::equal(name.begin(), name.end(), candidate.begin(), candidate.end(),
                       [&](char a, char b) { return fold(a) == fold(b); }))
            return std::string(candidate);
    return name;
}
const char* inventory =
    "SELECT r.standard_id,r.format_name,r.status,r.tymed,r.aspect,r.lindex,r.codec_version,"
    "r.blob_id,r.byte_count,b.size,b.relative_path FROM representations r LEFT JOIN blobs b ON "
    "b.id=r.blob_id "
    "WHERE r.snapshot_id=? ORDER BY r.ordinal LIMIT 256";
bool available(Database& db, const Statement& row) {
    if (row.is_null(9) || row.integer(8) != row.integer(9))
        return false;
    if (row.is_null(10))
        return true;
    try {
        const auto path = safe_blob_path(db.root(), row.text(7));
        std::error_code ec;
        return fs::file_size(path, ec) == static_cast<std::uintmax_t>(row.integer(9)) && !ec;
    } catch (const std::exception&) {
        return false;
    }
}
// A card names its first file from a bounded prefix. The list can be far longer.
std::vector<unsigned char> blob_prefix(Database& db, std::string_view hash, bool external, std::size_t limit) {
    std::vector<unsigned char> bytes;
    if (external) {
        std::ifstream in(safe_blob_path(db.root(), hash), std::ios::binary);
        bytes.resize(limit);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
        return bytes;
    }
    Statement query(db.handle(), "SELECT substr(inline_bytes,1,?) FROM blobs WHERE id=?");
    query.bind(1, static_cast<std::int64_t>(limit));
    query.bind(2, hash);
    if (query.step() && !query.is_null(0))
        bytes = query.blob(0);
    return bytes;
}
struct SavedList {
    std::string hash;
    bool external = false, descriptors = false;
    std::size_t length{};
};
} // namespace

Browser::Browser(HistoryStore& store) : db_(store.database()) {}
const std::filesystem::path& Browser::root() const { return db_.root(); }

std::vector<domain::Id> Browser::floating_ids(bool visible, int limit, std::string_view after) {
    std::lock_guard lock(db_.mutex());
    Statement rows(db_.handle(), "SELECT e.id FROM entries e JOIN card_presentation p ON p.entry_id=e.id "
                                 "WHERE e.state='Floating' AND p.visible=? AND e.id>? ORDER BY e.id LIMIT ?");
    rows.bind(1, int(visible));
    rows.bind(2, after);
    rows.bind(3, limit);
    std::vector<domain::Id> ids;
    while (rows.step()) ids.push_back(rows.text(0));
    return ids;
}

domain::Rope read_rope(Database& db, std::string_view id) {
    std::lock_guard lock(db.mutex());
    Statement row(db.handle(), "SELECT id,name,capacity,is_main,color_value,material_natural,tone_deep,tone_dark,tone_mid,tone_light,tone_high,texture_seed FROM ropes WHERE id=?");
    row.bind(1, id);
    if (!row.step()) throw std::out_of_range("Rope unavailable");
    return rope_from_row(row);
}
domain::Rope Browser::rope(std::string_view id) {
    return read_rope(db_, id);
}

std::size_t Browser::rope_count(bool include_hidden) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), include_hidden ? "SELECT count(*) FROM ropes" : "SELECT count(*) FROM navigable_ropes");
    if (!row.step()) throw std::runtime_error("Could not count ropes");
    return static_cast<std::size_t>(row.integer(0));
}

std::vector<domain::Rope> Browser::ropes(std::size_t offset, std::size_t limit) {
    if (offset > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) || limit > 256)
        throw std::invalid_argument("Invalid rope page");
    std::lock_guard lock(db_.mutex());
    Statement rows(db_.handle(), "SELECT id,name,capacity,is_main,color_value,material_natural,tone_deep,tone_dark,tone_mid,tone_light,tone_high,texture_seed FROM navigable_ropes ORDER BY sort_rank,id LIMIT ? OFFSET ?");
    rows.bind(1, static_cast<std::int64_t>(limit));
    rows.bind(2, static_cast<std::int64_t>(offset));
    std::vector<domain::Rope> result;
    while (rows.step()) result.push_back(rope_from_row(rows));
    return result;
}

std::optional<std::size_t> Browser::rope_position(std::string_view id) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT (SELECT count(*) FROM navigable_ropes p WHERE (p.sort_rank,p.id)<(r.sort_rank,r.id)) "
                                "FROM navigable_ropes r WHERE r.id=?");
    row.bind(1, id);
    if (!row.step()) return {};
    return static_cast<std::size_t>(row.integer(0));
}

std::size_t Browser::count(std::string_view rope_id) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT count(*) FROM entries WHERE state='Hanging' AND rope_id=?");
    row.bind(1, rope_id);
    if (!row.step()) throw std::runtime_error("Could not read rope length");
    return static_cast<std::size_t>(row.integer(0));
}

namespace {
void read_formats(Database& db, StoredCard& card, FileListExtent extent, PageReadStats* stats) {
    bool image_external = false, has_text = false;
    Statement rows(db.handle(), inventory);
    rows.bind(1, card.entry.snapshot_id);
    std::optional<SavedList> files;
    while (rows.step()) {
        const auto id = static_cast<UINT>(rows.integer(0));
        const auto name = canonical_name(rows.text(1));
        if (rows.text(2) != "saved") {
            card.partial = true;
            continue;
        }
        if (!available(db, rows)) {
            card.damaged = true;
            continue;
        }
        const auto medium=rows.integer(3);
        const bool medium_ok = name == "FileContents" ? medium==TYMED_ISTREAM && rows.integer(5)>=0 :
            rows.integer(5)==-1 && (medium==TYMED_HGLOBAL || (name=="PNG" && medium==TYMED_ISTREAM));
        if (!medium_ok || rows.integer(6) != 1 || rows.integer(4) != DVASPECT_CONTENT) {
            card.damaged = true;
            continue;
        }
        card.formats.emplace_back(id, name);
        has_text |= id == CF_UNICODETEXT;
        if (card.image_hash.empty() && (id == CF_DIB || id == CF_DIBV5 || name == "PNG")) {
            card.image_hash = rows.text(7);
            image_external = !rows.is_null(10);
            card.image_format = id;
        }
        // Real paths outrank virtual names. FileContents carries no names.
        if ((id == CF_HDROP && (!files || files->descriptors)) ||
            (name == "FileGroupDescriptorW" && !files))
            files = SavedList{rows.text(7), !rows.is_null(10), id != CF_HDROP, static_cast<std::size_t>(rows.integer(8))};
    }
    if (files) {
        constexpr std::size_t drop_prefix = 16 * 1024, descriptor_prefix = 4 + 592;
        const auto limit = files->descriptors ? descriptor_prefix : extent == FileListExtent::full ? max_group_bytes : drop_prefix;
        card.file_bytes = blob_prefix(db, files->hash, files->external, std::min(files->length, limit));
        if (stats) stats->prefix_bytes += card.file_bytes.size();
        card.file_hash = std::move(files->hash);
        card.file_length = files->length;
        card.descriptors = files->descriptors;
    }
    if (!has_text && !card.image_hash.empty()) {
        card.image_header = blob_prefix(db, card.image_hash, image_external, 32);
        if (stats) stats->prefix_bytes += card.image_header.size();
    }
}
} // namespace

StoredCard Browser::summary(std::string_view entry_id, FileListExtent extent) {
    std::lock_guard lock(db_.mutex());
    const std::string sql = std::string(summary_columns) + "WHERE e.id=?";
    Statement row(db_.handle(), sql.c_str());
    row.bind(1, prefix_bytes);
    row.bind(2, entry_id);
    if (!row.step()) throw std::out_of_range("Entry unavailable");
    auto result = read_summary(db_, row);
    read_formats(db_, result, extent, nullptr);
    return result;
}

std::vector<StoredCard> Browser::page(std::string_view rope_id, std::size_t offset,
                                                    std::size_t count, FileListExtent extent, PageReadStats* stats) {
    if (offset > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) || count > 256)
        throw std::invalid_argument("Invalid summary page");
    std::lock_guard lock(db_.mutex());
    const std::string sql = std::string(summary_columns) +
        "WHERE e.state='Hanging' AND e.rope_id=? ORDER BY e.sort_key,e.id LIMIT ? OFFSET ?";
    Statement rows(db_.handle(), sql.c_str());
    rows.bind(1, prefix_bytes);
    rows.bind(2, rope_id);
    rows.bind(3, static_cast<std::int64_t>(count));
    rows.bind(4, static_cast<std::int64_t>(offset));
    if (stats) *stats = {};
    std::vector<StoredCard> result;
    while (rows.step()) {
        auto card = read_summary(db_, rows, stats);
        read_formats(db_, card, extent, stats);
        result.push_back(std::move(card));
    }
    return result;
}

domain::PathKind Browser::saved_path_kind(const domain::Entry& expected) {
    std::lock_guard guard(db_.mutex());
    Statement row(db_.handle(), "SELECT s.path_kind FROM snapshots s JOIN entries e ON e.snapshot_id=s.id "
                                "WHERE e.id=? AND s.id=? AND e.revision=?");
    row.bind(1, expected.id);
    row.bind(2, expected.snapshot_id);
    row.bind(3, expected.revision);
    if (!row.step()) throw application::StaleEdit{};
    return static_cast<domain::PathKind>(row.integer(0));
}

std::vector<unsigned char> Browser::file_list(std::string_view hash) {
    std::vector<unsigned char> bytes;
    bool external{};
    size_t size{};
    {
        std::lock_guard guard(db_.mutex());
        Statement blob(db_.handle(), "SELECT relative_path,size,inline_bytes FROM blobs WHERE id=?");
        blob.bind(1, hash);
        if (!blob.step() || blob.integer(1) <= 0 || blob.integer(1) > max_group_bytes) return {};
        size = static_cast<size_t>(blob.integer(1));
        external = !blob.is_null(0);
        if (!external) bytes = blob.blob(2);
    }
    // Probe the filesystem outside the writer lock. Check identity again before committing.
    if (external) bytes = blob_prefix(db_, hash, true, size);
    if (sha256(bytes) != hash) return {};
    return bytes;
}

void HistoryStore::save_path_kind(const domain::Entry& entry, domain::PathKind kind) {
    std::lock_guard guard(db_.mutex());
    Statement save(db_.handle(), "UPDATE snapshots SET path_kind=? WHERE id=? AND path_kind=0 AND "
                                 "EXISTS(SELECT 1 FROM entries WHERE id=? AND revision=? AND snapshot_id=snapshots.id)");
    save.bind(1, int(kind));
    save.bind(2, entry.snapshot_id);
    save.bind(3, entry.id);
    save.bind(4, entry.revision);
    save.run();
    if (!sqlite3_changes(db_.handle())) (void)Browser(*this).saved_path_kind(entry); // Reject a stale binding.
}

domain::PathKind HistoryStore::confirm_menu_kind(const application::CardMenuSnapshot& menu) {
    Browser records(*this);
    const auto current = records.saved_path_kind(menu.entry);
    if (current != domain::PathKind::unknown || menu.observed_path_kind == domain::PathKind::unknown) return current;
    // A full command queue may leave the open popup as the only owner of this observation.
    save_path_kind(menu.entry, menu.observed_path_kind);
    return records.saved_path_kind(menu.entry);
}

application::CardDetails Browser::card_details(const application::CardSummary& card) {
    using Line = application::DetailLine;
    using Code = application::DetailText;
    std::lock_guard guard(db_.mutex());
    Statement head(db_.handle(), "SELECT datetime(s.captured_at,'unixepoch','localtime'),s.source_hint,s.capture_status "
                                 "FROM snapshots s JOIN entries e ON e.snapshot_id=s.id WHERE e.id=? AND s.id=?");
    head.bind(1, card.entry.id);
    head.bind(2, card.entry.snapshot_id);
    if (!head.step()) throw application::StaleEdit{};
    application::CardDetails result{{Line::heading, {}, {}, Code::capture_details},
        {Line::field, {}, wide(head.text(0)), Code::saved, Code::local_time},
        {Line::field, {}, wide(head.text(1)), Code::source, head.text(1).empty() ? Code::unknown : Code::literal},
        {Line::field, {}, wide(head.text(2)), Code::capture, Code::capture_status}};
    if (!card.file_list_hash.empty()) {
        using enum domain::FileAvailability;
        const auto state = card.availability;
        result.push_back({Line::heading, {}, {}, Code::file_health});
        Statement blob(db_.handle(), "SELECT relative_path,size FROM blobs WHERE id=?");
        blob.bind(1, card.file_list_hash);
        if (blob.step()) {
            const auto paths = file_group_paths(blob_prefix(db_, card.file_list_hash, !blob.is_null(0),
                                                static_cast<std::size_t>(blob.integer(1))));
            for (const auto& path : paths) result.push_back({Line::field, {}, path, Code::original_path});
        }
        result.push_back({Line::field, {}, {}, Code::file_status,
            state == missing ? Code::missing : state == inaccessible ? Code::inaccessible :
            state == on_request ? Code::refresh : state == unchecked ? Code::unchecked : Code::available});
        result.push_back({Line::field, {}, {}, Code::preview,
            card.file_preview == application::CardSummary::FilePreviewKind::none ? Code::not_applicable :
            state == decode_failed ? Code::decode_failed : card.thumbnail ?
            (state == available ? Code::ready : Code::cached) : Code::unavailable});
    }
    auto formats = inventory_lines(card.entry.id);
    result.insert(result.end(), formats.begin(), formats.end());
    return result;
}

application::CardDetails Browser::inventory_lines(std::string_view id) {
    using Line = application::DetailLine;
    using Code = application::DetailText;
    std::lock_guard guard(db_.mutex());
    Statement entry(db_.handle(), "SELECT s.id,s.capture_status FROM entries e JOIN snapshots s ON "
                                  "s.id=e.snapshot_id WHERE e.id=?");
    entry.bind(1, id);
    if (!entry.step())
        throw std::out_of_range("Card no longer exists");
    Statement rows(db_.handle(), inventory);
    rows.bind(1, entry.text(0));
    const bool partial = entry.text(1) == "partial";
    bool all_observed_saved = true;
    application::CardDetails result{{Line::heading, {}, {}, partial ? Code::partial_inventory : Code::inventory}};
    while (rows.step()) {
        const auto numeric = static_cast<unsigned>(rows.integer(0));
        auto name = wide(rows.text(1));
        auto name_code = Code::literal;
        if (name.empty()) {
            if (numeric == CF_UNICODETEXT)
                name = L"CF_UNICODETEXT";
            else if (numeric == CF_DIB)
                name = L"CF_DIB";
            else if (numeric == CF_DIBV5)
                name = L"CF_DIBV5";
            else if (numeric == CF_HDROP)
                name = L"CF_HDROP";
            else
                { name = std::to_wstring(numeric); name_code = Code::windows_format; }
        }
        auto status = wide(rows.text(2));
        all_observed_saved &= status == L"saved";
        if (status == L"saved" && !available(db_, rows))
            status = L"damaged or missing";
        result.push_back({Line::format, std::move(name), std::move(status), name_code, Code::format_status,
                          static_cast<int>(rows.integer(5))});
    }
    if (partial && all_observed_saved)
        result.push_back({Line::note, {}, {}, Code::incomplete_inventory});
    return result;
}

std::optional<std::size_t> Browser::position(std::string_view rope_id, std::string_view entry_id) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT (SELECT count(*) FROM entries p WHERE p.state='Hanging' "
        "AND p.rope_id=e.rope_id AND (p.sort_key,p.id)<(e.sort_key,e.id)) "
        "FROM entries e WHERE e.state='Hanging' AND e.rope_id=? AND e.id=?");
    row.bind(1, rope_id);
    row.bind(2, entry_id);
    if (!row.step()) return {};
    return static_cast<std::size_t>(row.integer(0));
}

application::Anchor Browser::load_anchor(std::string_view rope_id) {
    std::lock_guard lock(db_.mutex());
    Statement row(db_.handle(), "SELECT anchor_entry_id,offset FROM rope_view_state WHERE rope_id=?");
    row.bind(1, rope_id);
    return row.step() ? application::Anchor{row.text(0), row.real(1)} : application::Anchor{};
}

void HistoryStore::save_anchor(std::string_view rope_id, const application::Anchor& anchor) {
    if (!std::isfinite(anchor.offset)) throw std::invalid_argument("Invalid scroll offset");
    std::lock_guard lock(db_.mutex());
    Statement update(db_.handle(), "UPDATE rope_view_state SET anchor_entry_id=(SELECT id FROM entries "
        "WHERE id=? AND state='Hanging' AND rope_id=?), offset=? WHERE rope_id=?");
    update.bind(1, anchor.entry);
    update.bind(2, rope_id);
    update.bind(3, anchor.offset);
    update.bind(4, rope_id);
    update.run();
}
} // namespace cliprope::persistence
