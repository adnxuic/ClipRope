#pragma once
#include "../integration/resident.hpp"
#include "platform_win/tray.hpp"
#include "platform_win/tray_popup.hpp"
#include "platform_win/menu_timing.hpp"

namespace hidden_fixture {
inline HWND tray_menu(HWND listener, DWORD pid) {
    using cliprope::win::Tray;
    probe::require(PostMessageW(listener, Tray::callback, MAKELPARAM(64,64), MAKELPARAM(WM_CONTEXTMENU,Tray::icon_id)),
                   "Could not open the notification menu");
    HWND menu{};
    probe::pump_until([&] {
        menu = probe::owned_window(pid, L"ClipRope.TrayMenu");
        return menu && (GetWindowLongPtrW(menu,GWL_STYLE)&WS_VISIBLE) &&
               SendMessageW(menu,cliprope::win::menu_timing_query,WPARAM(cliprope::win::MenuStage::painted),0);
    }, "Tray popup unavailable");
    return menu;
}
inline void reveal(HWND menu) {
    const auto button = GetDlgItem(menu, cliprope::win::tray_command::reveal_hidden);
    probe::require(button && IsWindowEnabled(button) && PostMessageW(button,BM_CLICK,0,0),
                   "No enabled reveal command for the saved hidden card");
    probe::pump_until([&] { return !IsWindow(menu); }, "Tray command did not close");
}
}
