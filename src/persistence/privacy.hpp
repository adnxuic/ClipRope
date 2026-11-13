#pragma once
#include "application/capture.hpp"
#include "persistence/database.hpp"

namespace cliprope::persistence {
application::CapturePolicy read_privacy(Database &db);
void save_privacy(Database &db, const application::CapturePolicy &policy);
application::CapturePolicy save_exclusions(Database &db, const std::vector<std::string>& original,
                                           std::vector<std::string> names);
} // namespace cliprope::persistence
