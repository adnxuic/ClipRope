#pragma once
#include "persistence/database.hpp"
namespace cliprope::persistence {
inline bool has_preference_table(sqlite3* db, const char* name) {
    Statement row(db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?");
    row.bind(1, name);
    return row.step();
}
}
