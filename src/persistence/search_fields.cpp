#include "persistence/search_fields.hpp"
#include "platform_win/search_text.hpp"

namespace cliprope::persistence {
using application::SearchField;
void insert_origin_field(sqlite3* db, const domain::Id& id, std::string_view path) {
    application::SearchFields origin;
    win::append_search_metadata(origin, path, {});
    if (origin.empty()) throw std::runtime_error("Invalid screenshot origin");
    Statement position(db, "SELECT coalesce(min(CASE WHEN kind=? THEN ordinal END),max(ordinal)+1,0) "
                           "FROM search_fields WHERE snapshot_id=?");
    position.bind(1, int(SearchField::metadata));
    position.bind(2, id);
    if (!position.step()) throw StorageFailure(StorageProblem::damaged_database);
    const auto ordinal = position.integer(0);
    Statement shift(db, "UPDATE search_fields SET ordinal=ordinal+1 WHERE snapshot_id=? AND kind=?");
    shift.bind(1, id);
    shift.bind(2, int(SearchField::metadata));
    shift.run();
    Statement add(db, "INSERT INTO search_fields VALUES(?,?,?,?)");
    add.bind(1, id);
    add.bind(2, ordinal);
    add.bind(3, int(SearchField::path));
    add.bind(4, origin.front().second);
    add.run();
}
void index_search_fields(sqlite3* db, const domain::Id& id, const SearchFields& fields) {
    int ordinal = 0;
    for (auto& [kind, value] : fields) {
        Statement stmt(db, "INSERT INTO search_fields VALUES(?,?,?,?)");
        stmt.bind(1, id);
        stmt.bind(2, ordinal++);
        stmt.bind(3, int(kind));
        stmt.bind(4, value);
        stmt.run();
    }
}
} // namespace cliprope::persistence
