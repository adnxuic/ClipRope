#pragma once
#include "application/card_search.hpp"
#include "persistence/database.hpp"

namespace cliprope::persistence {
using application::SearchFields;
// Insert the origin immediately before the optional source field, as in extract_search_fields.
void insert_origin_field(sqlite3*, const domain::Id&, std::string_view screenshot_path);
void index_search_fields(sqlite3* db, const domain::Id& id, const SearchFields& fields);
} // namespace cliprope::persistence
