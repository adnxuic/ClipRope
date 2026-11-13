#pragma once
#include "history_fixture.hpp"
#include "persistence/database.hpp"
#include <memory>

namespace history_fixture {
// A separate SQLite connection observes the synthetic profile while the desktop owns its process lock.
inline auto observe(const std::filesystem::path& profile, int flags = SQLITE_OPEN_READONLY) {
    const auto path = (profile / "history.db").u8string();
    sqlite3* raw{};
    const auto result = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, flags, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    require(result == SQLITE_OK, "Could not open fixture observation connection");
    sqlite3_busy_timeout(raw, 500);
    return db;
}
}
