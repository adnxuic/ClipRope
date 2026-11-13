#pragma once
#include <array>
#include <string>

namespace cliprope::application {
// Values are durable action IDs in shortcut_preferences.
enum ShortcutAction : unsigned { workspace_toggle, quick_note, next_rope, toggle_overview, find_cards, shortcut_count };
struct ShortcutBinding {
    bool enabled = true;
    std::string chord;
    bool operator==(const ShortcutBinding&) const = default;
};
using ShortcutBindings = std::array<ShortcutBinding, shortcut_count>;
inline ShortcutBindings default_shortcuts() {
    return {{{true, "Ctrl+Shift+V"}, {true, "Ctrl+Shift+Space"}, {true, "Tab"}, {true, "Ctrl+Tab"}, {true, "Ctrl+F"}}};
}
}
