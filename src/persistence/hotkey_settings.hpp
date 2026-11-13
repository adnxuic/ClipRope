#pragma once
#include "persistence/database.hpp"
#include "application/shortcuts.hpp"
#include "application/card_preferences.hpp"
#include "application/appearance.hpp"

namespace cliprope::persistence {
application::CardPreferences read_card_preferences(Database& db);
void save_card_preferences(Database& db, const application::CardPreferences& value);
bool read_workspace_topmost(Database& db);
void save_workspace_topmost(Database& db, bool topmost);
// The receipt exists only while a reset may have enabled a previously absent Run entry.
bool pending_sign_in_rollback(Database& db);
void mark_sign_in_rollback(Database& db, bool pending);
void reset_global_preferences(Database& db, bool finish_sign_in = false);
application::Appearance read_appearance(Database& db);
void save_appearance(Database& db, application::Appearance value);
void save_match_color(Database& db, domain::ColorSelection original, domain::ColorSelection selected);
application::ShortcutBindings read_shortcuts(Database& db);
void save_shortcut(Database& db, application::ShortcutAction action, const application::ShortcutBinding& binding);
}
