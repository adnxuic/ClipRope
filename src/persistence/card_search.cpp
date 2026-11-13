#include "persistence/card_search.hpp"
#include <algorithm>

namespace cliprope::persistence {
namespace {
using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
Connection open(const std::filesystem::path& root) {
    check_closed_history(root);
    auto path = (root / "history.db").u8string();
    sqlite3* raw{};
    auto rc = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, SQLITE_OPEN_READONLY, nullptr);
    Connection db(raw, sqlite3_close);
    if (rc != SQLITE_OK) throw StorageFailure(StorageProblem::access);
    sqlite3_busy_timeout(raw, 3000);
    require_current_history(raw);
    return db;
}
void count_matches(sqlite3_context* ctx, int, sqlite3_value** args) {
    const auto* document = static_cast<const wchar_t*>(sqlite3_value_text16(args[0]));
    const auto* term = static_cast<const wchar_t*>(sqlite3_value_text16(args[1]));
    if (!document || !term) {
        sqlite3_result_error_nomem(ctx);
        return;
    }
    std::wstring_view text(document, sqlite3_value_bytes16(args[0]) / 2);
    std::wstring_view query(term, sqlite3_value_bytes16(args[1]) / 2);
    sqlite3_int64 count = 0;
    const auto* stop = static_cast<const std::stop_token*>(sqlite3_user_data(ctx));
    if (!query.empty()) {
        for (size_t i = 0; i + query.size() <= text.size();) {
            if (!(i % 4096) && stop->stop_requested()) {
                sqlite3_result_error_code(ctx, SQLITE_INTERRUPT);
                return;
            }
            const bool equal = application::find_fold(text[i]) == query.front() &&
                               std::equal(query.begin() + 1, query.end(), text.begin() + i + 1,
                                          [](wchar_t a, wchar_t b) { return a == application::find_fold(b); });
            if (equal) {
                ++count;
                i += query.size();
            } else
                ++i;
        }
    }
    sqlite3_result_int64(ctx, count);
}
struct Interrupt {
    sqlite3* db;
    std::stop_token stop;
    Interrupt(sqlite3* db, std::stop_token s) : db(db), stop(s) {
        sqlite3_progress_handler(
            db, 500, [](void* p) { return int(static_cast<Interrupt*>(p)->stop.stop_requested()); }, this);
    }
    ~Interrupt() {
        sqlite3_progress_handler(db, 0, nullptr, nullptr);
    }
};
} // namespace
CardSearch::CardSearch(const std::filesystem::path& root) : db_(open(root)) {
    if (sqlite3_create_function_v2(db_.get(), "find_count", 2, SQLITE_UTF16 | SQLITE_DETERMINISTIC, &query_stop_,
                                   count_matches, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare search");
}
SearchResults CardSearch::query(const application::FindScope& scope, std::wstring term, std::stop_token stop) {
    page_ = {}; // Binding an origin can shift the metadata field on an existing snapshot.
    SearchResults result;
    application::fold_find(term);
    result.folded = std::move(term);
    if (result.folded.empty()) return result;
    Interrupt interrupt(db_.get(), stop);
    query_stop_ = stop;
    Statement rows(db_.get(),
                   "SELECT e.id,e.snapshot_id,e.rope_id,f.ordinal,f.kind,find_count(f.search_text,?4) FROM ropes r "
                   "JOIN entries e ON e.rope_id=r.id AND e.state='Hanging' "
                   "JOIN search_fields f ON f.snapshot_id=e.snapshot_id "
                   "WHERE (?1 OR r.id=?2) AND (?3 OR r.id<>'system:rebin') "
                   "ORDER BY (r.sort_rank,r.id)<(SELECT sort_rank,id FROM ropes WHERE "
                   "id=?2),r.sort_rank,r.id,e.sort_key,e.id,f.ordinal");
    rows.bind(1, int(scope.overview));
    rows.bind(2, scope.first_rope);
    rows.bind(3, int(scope.include_rebin));
    rows.bind(4, result.folded);
    while (!stop.stop_requested() && rows.step()) {
        auto count = size_t(rows.integer(5));
        if (count) {
            result.groups.push_back({rows.text(0), rows.text(1), rows.text(2), int(rows.integer(3)),
                                     application::SearchField(rows.integer(4)), result.total, count});
            result.total += count;
        }
    }
    return result;
}
std::optional<application::SearchHit> CardSearch::locate(const SearchResults& results, size_t index,
                                                         std::stop_token stop) {
    if (index >= results.total) return {};
    auto it = std::upper_bound(results.groups.begin(), results.groups.end(), index,
                               [](size_t n, const MatchGroup& group) { return n < group.begin; }) -
              1;
    Statement live(db_.get(), "SELECT 1 FROM entries WHERE id=? AND snapshot_id=? AND rope_id=? AND state='Hanging'");
    live.bind(1, it->entry);
    live.bind(2, it->snapshot);
    live.bind(3, it->rope);
    if (!live.step()) return {};
    const size_t occurrence = index - it->begin, length = results.folded.size();
    const bool same = page_.snapshot == it->snapshot && page_.ordinal == it->ordinal && page_.term == results.folded;
    if (!same || occurrence < page_.first || occurrence - page_.first >= page_.hits.size()) {
        Statement field(db_.get(), "SELECT search_text FROM search_fields WHERE snapshot_id=? AND ordinal=?");
        field.bind(1, it->snapshot);
        field.bind(2, it->ordinal);
        if (!field.step()) return {};
        auto original = field.wide(0), folded = original;
        application::fold_find(folded);
        // Retain at most one small position page. Forward traversal continues
        // at the previous page's trailing offset, even on a frequent one-letter query.
        const size_t page_size = std::max(size_t(1), std::min(size_t(64), 8192 / (length + 144)));
        PositionPage next{it->snapshot, it->ordinal, results.folded, occurrence / page_size * page_size};
        const bool forward = same && next.first >= page_.first + page_.hits.size();
        size_t p = forward ? page_.next_offset : 0;
        const size_t begin = forward ? page_.first + page_.hits.size() : 0;
        for (size_t n = begin; n < std::min(it->count, next.first + page_size); ++n) {
            const auto previous_end = p;
            p = folded.find(results.folded, p);
            if (p == folded.npos || stop.stop_requested()) return {};
            if (n >= next.first) {
                // Starting inside the preceding occurrence can shift the highlights
                // for self-overlapping literals such as "aaaaa".
                size_t first = std::max(previous_end, p > 48 ? p - 48 : 0);
                size_t end = std::min(original.size(), p + length + 96);
                if (first && original[first] >= 0xdc00 && original[first] <= 0xdfff) --first;
                if (end < original.size() && original[end] >= 0xdc00 && original[end] <= 0xdfff) ++end;
                next.hits.push_back({{p, length}, original.substr(first, end - first), first});
            }
            p += length;
        }
        next.next_offset = p;
        page_ = std::move(next);
    }
    const auto& match = page_.hits.at(occurrence - page_.first);
    return application::SearchHit{it->entry,   it->snapshot, it->rope,    it->ordinal, it->field,
                                  match.range, match.text,   match.start, occurrence};
}
} // namespace cliprope::persistence
