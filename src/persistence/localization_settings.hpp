#pragma once
#include "localization/localization.hpp"
#include "persistence/database.hpp"

namespace cliprope::persistence {
localization::Language read_language(Database& db);
void save_language(Database& db, localization::Language language);
} // namespace cliprope::persistence
