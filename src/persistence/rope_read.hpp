#pragma once
#include "domain/history.hpp"
#include "persistence/database.hpp"

namespace cliprope::persistence {
void require_rope_name(std::string_view value);
domain::Rope read_rope(Database&, std::string_view id);
}
