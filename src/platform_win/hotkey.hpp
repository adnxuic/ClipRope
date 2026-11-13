#pragma once
#include "application/shortcuts.hpp"
#include "platform_win/workspace_commands.hpp"
#include "platform_win/localization.hpp"
#include <optional>
#include <windows.h>

namespace cliprope::win {
struct Hotkey {
    UINT modifiers{};
    UINT key{};
    bool operator==(const Hotkey&) const = default;
};
std::optional<Hotkey> parse_hotkey(std::string_view text);
std::string hotkey_name(Hotkey key);

inline const UiText shortcut_labels[] = {
    tr(TextKey::hotkey_workspace), tr(TextKey::tray_quick_note), tr(TextKey::hotkey_next_rope), tr(TextKey::hotkey_overview), tr(TextKey::find_title)
};
inline bool global_shortcut(application::ShortcutAction action) { return action < application::next_rope; }
bool valid_hotkey(Hotkey chord, bool global);
inline UINT current_modifiers() {
    return (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) |
           (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
           (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0 ? MOD_WIN : 0);
}
using ScopedKeys = std::array<Hotkey, 3>;
inline constexpr ScopedKeys default_scoped_keys{{{0,VK_TAB}, {MOD_CONTROL,VK_TAB}, {MOD_CONTROL,'F'}}};
inline WorkspaceShortcut match_scoped(const ScopedKeys& bindings, Hotkey chord) {
    constexpr WorkspaceShortcut commands[]{WorkspaceShortcut::next_rope, WorkspaceShortcut::toggle_overview,
                                           WorkspaceShortcut::find};
    for (size_t i = 0; i < bindings.size(); ++i)
        if (chord.key && bindings[i] == chord) return commands[i];
    return WorkspaceShortcut::none;
}
}
