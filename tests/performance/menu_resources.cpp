#include "platform_win/card_menu_frame.hpp"
#include "platform_win/popup_menu.hpp"
#include "platform_win/tray_popup.hpp"
#include <iostream>
#include <ole2.h>
#include <stdexcept>

using namespace cliprope::win;
namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
void CALLBACK dismiss(HWND, UINT, UINT_PTR id, DWORD) {
    if (auto popup = FindWindowW(L"ClipRope.CardMenu", nullptr)) {
        KillTimer(nullptr, id);
        PostMessageW(popup, WM_CLOSE, 0, 0);
    }
}
unsigned paints{}, idle_paints{}, ticks{};
bool common_visible{}, idle{}, resolved{};
LRESULT CALLBACK count_paints(int code, WPARAM w, LPARAM l) {
    if (code >= 0 && reinterpret_cast<CWPSTRUCT*>(l)->message == WM_PAINT) ++paints;
    return CallNextHookEx(nullptr, code, w, l);
}
void CALLBACK inspect_pending(HWND, UINT, UINT_PTR timer, DWORD) {
    auto menu = FindWindowW(L"ClipRope.CardMenu", nullptr);
    if (!menu) return;
    ++ticks;
    RECT bounds{};
    GetClientRect(menu, &bounds);
    const auto dpi = fluent::design_dpi(GetDpiForWindow(menu));
    const bool fits = bounds.right == MulDiv(248, dpi, 96) &&
                      bounds.bottom == MulDiv(48 + 24 * (GetDlgItem(menu, 2) ? 2 : 1) + 8, dpi, 96);
    if (ticks == 1) {
        common_visible = fits && GetDlgItem(menu, 1) && !GetDlgItem(menu, 2);
        idle_paints = paints;
    } else if (ticks == 2) {
        idle = idle_paints == paints;
        PopupMenuHost::file_preview_ready();
    } else {
        resolved = fits && GetDlgItem(menu, 2) != nullptr;
        KillTimer(nullptr, timer);
        PostMessageW(menu, WM_CLOSE, 0, 0);
    }
}
void check_pending(HWND owner) {
    auto hook = SetWindowsHookExW(WH_CALLWNDPROC, count_paints, nullptr, GetCurrentThreadId());
    auto timer = SetTimer(nullptr, 0, 120, inspect_pending);
    check(hook && timer, "Pending menu observation unavailable");
    PopupMenuHost::track(owner, {60, 60}, L"Settings", L"Unclassified reference", {{1, L"Common action", 0}},
        PopupFace::card, [](UiText& label, std::vector<PopupItem>& rows) {
            if (ticks < 2) return false;
            label = L"Folder";
            rows.push_back({2, L"Resolved action", 0});
            return true;
        });
    KillTimer(nullptr, timer);
    UnhookWindowsHookEx(hook);
    check(common_visible && resolved && idle, "Pending menu did not settle or complete its existing window");
}
RECT tray_face{};
UINT tray_dpi{};
void CALLBACK measure_tray(HWND, UINT, UINT_PTR timer, DWORD) {
    if (auto popup = FindWindowW(L"ClipRope.TrayMenu", nullptr)) {
        GetWindowRect(popup, &tray_face);
        tray_dpi = fluent::design_dpi(GetDpiForWindow(popup));
        KillTimer(nullptr, timer);
        PostMessageW(popup, WM_CLOSE, 0, 0);
    }
}
void check_tray_edges(HWND owner) {
    MONITORINFO display{sizeof(display)};
    check(GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &display), "Work area unavailable");
    const RECT work = display.rcWork;
    for (bool top : {false, true}) {
        for (bool right : {false, true}) {
            const LONG x = right ? work.right - 30 : work.left + 30;
            const LONG y = top ? work.top : work.bottom - 20;
            RECT icon{x, y, x + 20, y + 20};
            const auto timer = SetTimer(nullptr, 0, 15, measure_tray);
            check(timer != 0, "Tray placement timer unavailable");
            tray_face = {};
            PopupMenuHost::track(owner, {x,y}, L"Tray", L"Position",
                {{tray_command::open,L"Test"}, {tray_command::first_preview,L"Sample"},
                 {tray_command::quick_note,L"Quick note"}, {tray_command::settings,L"Settings"}, {tray_command::exit,L"Exit"}},
                PopupFace::tray, {}, nullptr, &icon);
            KillTimer(nullptr,timer);
            const int gap = MulDiv(2,tray_dpi,96);
            check(!IsRectEmpty(&tray_face) && (top ? tray_face.top == icon.bottom + gap : tray_face.bottom == y - gap)
                  && (right ? tray_face.right == work.right : tray_face.left == x), "Incorrect tray edge alignment");
        }
    }
}
void check_resources(HWND owner) {
    DWORD gdi{}, user{};
    for (int i = 0; i < 80; ++i) {
        auto timer = SetTimer(nullptr, 0, 10, dismiss);
        check(timer != 0, "Menu timer unavailable");
        PopupMenuHost::track(owner, {60, 60}, L"Settings", L"Synthetic card",
                             {{1, L"Test action", 0xe790}}, PopupFace::card);
        KillTimer(nullptr, timer);
        check(fluent::Surface::target_creations() == 1, "Repeated menu recreated the render target");
        check(CardMenuFrame::generations() == 1, "Repeated menu rasterized the frame");
        if (i % 10 == 9) {
            Sleep(100);
            GdiFlush();
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        }
        if (i == 39) {
            gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        }
        check(fluent::Surface::control_artwork_bytes() <= fluent::Surface::artwork_budget, "Artwork cache exceeded its budget");
        if (i == 79) {
            std::cout << "Closed menu GDI " << gdi << " -> " << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)
                      << ", USER " << user << " -> " << GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) << '\n';
            check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi &&
                  GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user, "Menu GUI handles grew");
        }
    }
    fluent::Surface::discard_target();
    auto timer = SetTimer(nullptr, 0, 10, dismiss);
    check(timer != 0, "Menu timer unavailable");
    PopupMenuHost::track(owner, {60, 60}, L"Settings", L"Changed header after device loss",
                         {{1, L"Test action", 0xe790}}, PopupFace::card);
    KillTimer(nullptr, timer);
    check(fluent::Surface::target_creations() == 2 && CardMenuFrame::generations() == 1,
          "Device loss discarded independent resources or failed to replace the target");
    auto palette = fluent::system_palette();
    CardMenuFrame frame;
    check(frame.open(owner), "Test frame unavailable");
    for (UINT dpi : {96u, 120u, 144u, 192u}) {
        for (int color = 0; color < 4; ++color) {
            palette.panel = RGB(16, color, 128);
            const auto before = CardMenuFrame::generations();
            check(frame.fit(owner, palette, dpi) && frame.fit(owner, palette, dpi), "Frame fit failed");
            check(CardMenuFrame::generations() == before + 1, "Frame palette/DPI key was ignored");
            check(CardMenuFrame::cache_bytes() <= CardMenuFrame::cache_budget, "Frame cache exceeded its budget");
        }
    }
}
}
int main() {
    auto desktop = CreateDesktopW((L"ClipRope.MenuTest." + std::to_wstring(GetCurrentProcessId())).c_str(),
                                  nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) return 2;
    auto owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Synthetic owner", WS_POPUP,
                                 0, 0, 640, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int result = 0;
    try { check(owner != nullptr, "Test window unavailable"); check_resources(owner); check_pending(owner); check_tray_edges(owner); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (owner) DestroyWindow(owner);
    OleUninitialize();
    return result;
}
