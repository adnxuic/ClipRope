#pragma once

namespace cliprope::win {
enum class WorkspaceCommand : unsigned { none, previous = 102, next = 103, head = 2100, latest, overview, keyboard, density, rebin, find };
enum class WorkspaceShortcut : unsigned { none, next_rope, previous_rope, toggle_overview, find };
struct WorkspaceMenuState { bool overview, compact, has_cards, rebin_visible = false, multiple = false; };
}
