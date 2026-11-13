#pragma once
#include "platform_win/app_icon_resource.h"
#include <windows.h>
#include <array>

namespace cliprope::win {
// UI-thread cache: HWNDs and Explorer borrow these handles until process shutdown.
// Keep distinct owned handles per size; LR_SHARED can return a cached wrong size.
inline HICON application_icon(int pixels) noexcept {
    constexpr std::array sizes{16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    struct Cache {
        std::array<HICON, sizes.size()> icons{};
        ~Cache() { for (auto icon : icons) if (icon) DestroyIcon(icon); }
    };
    static Cache cache;
    std::size_t index = 0;
    while (index + 1 < sizes.size() && sizes[index] < pixels) ++index;
    auto& icon = cache.icons[index];
    if (!icon)
        icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_CLIPROPE),
                                           IMAGE_ICON, sizes[index], sizes[index], LR_DEFAULTCOLOR));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

inline void set_application_icons(HWND window, UINT dpi = 0) noexcept {
    if (!IsWindow(window)) return;
    if (!dpi) dpi = GetDpiForWindow(window);
    SendMessageW(window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(application_icon(GetSystemMetricsForDpi(SM_CXICON, dpi))));
    SendMessageW(window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(application_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi))));
}

inline HICON notification_icon() noexcept {
    const auto taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const auto dpi = taskbar ? GetDpiForWindow(taskbar) : GetDpiForSystem();
    return application_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi));
}
} // namespace cliprope::win
