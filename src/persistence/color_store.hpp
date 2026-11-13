#pragma once
#include "persistence/database.hpp"
#include "domain/color.hpp"

namespace cliprope::persistence {
// These helpers participate in the caller's existing transaction.
void write_rope_material(sqlite3 *db, std::string_view id, domain::ColorSelection color,
                         const domain::RopeMaterial &material);
std::vector<domain::ColorRGBA8> recent_colors(Database &db);
void remember_color(sqlite3 *db, domain::ColorSelection color);
} // namespace cliprope::persistence
