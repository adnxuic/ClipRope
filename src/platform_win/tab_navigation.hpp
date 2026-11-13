#pragma once
#include "platform_win/hotkey.hpp"
#include <windows.h>
#include <array>

namespace cliprope::win {
// Bind a main key to its initial recipient until release, including after reconfiguration.
class TabNavigation final {
public:
    struct Route { WorkspaceShortcut command{}; bool consume{}; };
    void seed(UINT key, bool down) {
        if (key < down_.size()) down_[key] = down;
    }
    Route key(UINT key, bool down, bool eligible, const ScopedKeys& bindings = default_scoped_keys) {
        if (key >= down_.size() || (!down_[key] && !modifier(key) && !bound(key, bindings))) return {};
        const bool repeated = down_[key];
        down_[key] = down;
        if (!down) {
            const bool owned = consumed_[key];
            consumed_[key] = false;
            return {WorkspaceShortcut::none, owned};
        }
        if (repeated) return {WorkspaceShortcut::none, consumed_[key]};
        if (!eligible) return {};
        UINT modifiers = (held(VK_CONTROL, VK_LCONTROL, VK_RCONTROL) ? MOD_CONTROL : 0) |
                         (held(VK_SHIFT, VK_LSHIFT, VK_RSHIFT) ? MOD_SHIFT : 0) |
                         (held(VK_MENU, VK_LMENU, VK_RMENU) ? MOD_ALT : 0) |
                         (down_[VK_LWIN] || down_[VK_RWIN] ? MOD_WIN : 0);
        auto command = match_scoped(bindings, {modifiers, key});
        consumed_[key] = command != WorkspaceShortcut::none;
        return {command, consumed_[key]};
    }
    void pass_press(UINT key = VK_TAB) { consumed_[key] = false; }
    static bool modifier(UINT key) {
        return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN ||
               (key >= VK_LSHIFT && key <= VK_RMENU);
    }
    static bool bound(UINT key, const ScopedKeys& keys) {
        for (auto chord : keys) if (chord.key && chord.key == key) return true;
        return false;
    }
private:
    bool held(UINT generic, UINT left, UINT right) const {
        return down_[generic] || down_[left] || down_[right];
    }
    std::array<bool, 256> down_{};
    std::array<bool, 256> consumed_{};
};
}
