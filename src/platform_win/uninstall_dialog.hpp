#pragma once
#include "platform_win/build_uninstall.hpp"
#include <functional>

namespace cliprope::win {
struct UninstallActions {
    std::function<BuildUninstallPreview()> inspect;
    std::function<BuildUninstallResult(bool)> remove;
};
inline constexpr int uninstall_history_id = 2450, uninstall_status_id = 2451;
bool uninstall_dialog(HWND owner, UninstallActions actions);
} // namespace cliprope::win
