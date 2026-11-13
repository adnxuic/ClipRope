#include "persistence/privacy.hpp"
#include <windows.h>
#include <algorithm>
#include <stdexcept>

namespace cliprope::persistence {
namespace {
void check_basename(std::string_view name) {
    if (name.empty() || name.size() > 1020 || name == "." || name == ".." ||
        name.find_first_of("\\/:*?\"<>|") != name.npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Enter executable names without paths or wildcards.");
    const auto units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                           static_cast<int>(name.size()), nullptr, 0);
    if (!units || units > 255)
        throw std::invalid_argument("Invalid executable name.");
}
} // namespace
application::CapturePolicy read_privacy(Database &db) {
    std::lock_guard lock(db.mutex());
    Statement exists(db.handle(),
                     "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='capture_preferences'");
    if (!exists.step())
        return {};
    Statement row(
        db.handle(),
        "SELECT paused,CASE WHEN typeof(exclusions)='text' AND length(CAST(exclusions AS BLOB))<=65343 "
        "THEN exclusions END FROM capture_preferences WHERE id=1 AND typeof(paused)='integer'");
    if (!row.step() || row.is_null(1) || row.integer(0) < 0 || row.integer(0) > 1)
        throw std::runtime_error("Capture preferences are invalid.");
    application::CapturePolicy policy{row.integer(0) != 0, {}};
    const auto names = row.text(1);
    for (std::string_view rest(names); !rest.empty();) {
        const auto end = rest.find('\n');
        auto name = rest.substr(0, end);
        check_basename(name);
        if (policy.excluded_apps.size() == 64)
            throw std::runtime_error("Too many capture exclusions.");
        policy.excluded_apps.emplace_back(name);
        if (end == rest.npos)
            break;
        rest.remove_prefix(end + 1);
    }
    return policy;
}
namespace {
void write_privacy(Database &db, const application::CapturePolicy &policy) {
    if (policy.excluded_apps.size() > 64)
        throw std::invalid_argument("Use at most 64 executable names.");
    std::string names;
    for (const auto &app : policy.excluded_apps) {
        check_basename(app);
        if (!names.empty())
            names += '\n';
        names += app;
    }
    exec(db.handle(), "CREATE TABLE IF NOT EXISTS capture_preferences("
                      "id INTEGER PRIMARY KEY CHECK(id=1),paused INTEGER NOT NULL CHECK(paused IN(0,1)),"
                      "exclusions TEXT NOT NULL)");
    Statement row(db.handle(), "INSERT INTO capture_preferences VALUES(1,?,?) ON CONFLICT(id) "
                               "DO UPDATE SET paused=excluded.paused,exclusions=excluded.exclusions");
    row.bind(1, static_cast<int>(policy.paused));
    row.bind(2, names);
    row.run();
}
} // namespace

void save_privacy(Database &db, const application::CapturePolicy &policy) {
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    write_privacy(db, policy);
    tx.commit();
}

application::CapturePolicy save_exclusions(Database &db, const std::vector<std::string>& original,
                                           std::vector<std::string> names) {
    std::lock_guard lock(db.mutex());
    Transaction tx(db.handle());
    auto policy = read_privacy(db);
    if (policy.excluded_apps != original)
        throw std::runtime_error("The saved list has changed. Reopen the editor to load it.");
    policy.excluded_apps = std::move(names);
    write_privacy(db, policy);
    tx.commit();
    return policy;
}
} // namespace cliprope::persistence
