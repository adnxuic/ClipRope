#include "platform_win/fluent.hpp"
#include "platform_win/menu_bitmap.hpp"
#include <commctrl.h>
#include <ole2.h>
#include <iostream>
#include <stdexcept>

using namespace cliprope::win;
using namespace cliprope::win::fluent;
namespace {
int clicks{};
unsigned animation_ticks{};
bool synthetic_hover{};
LRESULT CALLBACK count_animation(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_TIMER) ++animation_ticks;
    // This private desktop has no physical cursor. The fixture controls departure.
    if (m == WM_MOUSELEAVE && synthetic_hover) return 0;
    return DefSubclassProc(h,m,w,l);
}
void settle(unsigned duration = 150) {
    const auto until = GetTickCount64() + duration;
    do {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(2);
    } while (GetTickCount64() < until);
}
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
LRESULT CALLBACK host_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_COMMAND && HIWORD(w) == BN_CLICKED) {
        ++clicks;
        return 0;
    }
    if (msg == WM_SETTINGCHANGE) {
        auto surface = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (surface) surface->refresh();
    }
    return DefWindowProcW(h, msg, w, l);
}
void check_pixels(HWND h, Surface& surface, const CardInk& ink, const char* state, bool hot = true) {
    RECT r{};
    GetClientRect(h, &r);
    MenuBitmap actual, expected;
    check(actual.create(r.right, r.bottom) && expected.create(r.right, r.bottom), "Cannot allocate test bitmaps");
    // Sample the existing surface before any paint, print or message pump can repair it.
    auto dc = GetDC(h);
    auto copied = BitBlt(actual.dc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    ReleaseDC(h, dc);
    check(copied != FALSE, "Cannot sample the button surface");
    surface.draw_card(h, expected.dc, r, ink, hot);
    GdiFlush();
    int different = 0;
    for (int i = 0; i < r.right * r.bottom; ++i)
        different += ((actual.pixels[i] ^ expected.pixels[i]) & 0xffffff) != 0;
    if (different)
        throw std::runtime_error("Part " + std::to_string(int(ink.part)) + " " + state + ": " +
                                 std::to_string(different) + " pixels differ from Fluent artwork");
}
void check_gdi_button(HWND host, Surface& surface) {
    auto h = child(host, L"BUTTON", L"Native caption", WS_TABSTOP, 11);
    check(h != nullptr, "Cannot create GDI button");
    // A swatch gives screen and memory DCs the same geometry without font smoothing differences.
    surface.style(h, {Button::swatch, 0, true, false, RGB(120, 65, 30)});
    MoveWindow(h, 0, 0, 40, 44, TRUE);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    MenuBitmap visible, printed, notified;
    check(visible.create(40, 44) && printed.create(40, 44) && notified.create(40, 44), "Cannot allocate GDI bitmaps");
    auto dc = GetDC(h);
    auto copied = BitBlt(visible.dc, 0, 0, 40, 44, dc, 0, 0, SRCCOPY);
    ReleaseDC(h, dc);
    check(copied != FALSE, "Cannot read GDI button");
    SendMessageW(h, WM_PRINTCLIENT, WPARAM(printed.dc), PRF_CLIENT);
    NMCUSTOMDRAW draw{};
    draw.hdr = {h, 11, NM_CUSTOMDRAW};
    draw.dwDrawStage = CDDS_PREPAINT;
    draw.hdc = notified.dc;
    GetClientRect(h, &draw.rc);
    check(SendMessageW(host, WM_NOTIFY, 11, LPARAM(&draw)) == CDRF_SKIPDEFAULT, "Native default paint was allowed");
    GdiFlush();
    int print_delta = 0, notify_delta = 0;
    for (int i = 0; i < 40 * 44; ++i) {
        print_delta += ((visible.pixels[i] ^ printed.pixels[i]) & 0xffffff) != 0;
        notify_delta += ((printed.pixels[i] ^ notified.pixels[i]) & 0xffffff) != 0;
    }
    if (print_delta || notify_delta)
        throw std::runtime_error("GDI window/print differences: " + std::to_string(print_delta) +
                                 ", print/notify differences: " + std::to_string(notify_delta));
    SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    DestroyWindow(h);
}
void check_color_states(HWND host, Surface& surface) {
    auto h = child(host,L"BUTTON",L"Color",WS_TABSTOP,12);
    check(h != nullptr,"Cannot create color button");
    surface.card_style(h,{CardPart::color_picker,0,L"#C9A26B",{},{},RGB(201,162,107)});
    SetWindowSubclass(h,count_animation,77,0);
    MoveWindow(h,0,60,180,36,TRUE);
    MenuBitmap sample;
    check(sample.create(180,36),"Cannot allocate state capture");
    auto hash = [&] {
        SendMessageW(h,WM_PRINTCLIENT,WPARAM(sample.dc),PRF_CLIENT);
        GdiFlush();
        unsigned long long value = 1469598103934665603ull;
        for (int i=0;i<180*36;++i) value = (value ^ (sample.pixels[i]&0xffffff)) * 1099511628211ull;
        return value;
    };
    const auto normal = hash();
    synthetic_hover = true;
    SendMessageW(h,WM_MOUSEMOVE,0,MAKELPARAM(12,12));
    settle();
    const auto hovered = hash();
    check(normal != hovered,"Color button has no hover feedback");
    SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(12,12));
    settle();
    check(hovered != hash(),"Color button has no pressed feedback");
    SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(12,12));
    EnableWindow(h,FALSE);
    settle();
    const auto disabled = hash();
    synthetic_hover = false;
    SendMessageW(h,WM_MOUSELEAVE,0,0);
    settle();
    check(disabled == hash(),"Disabled color button reacts to hover");
    const auto ticks = animation_ticks;
    settle();
    check(ticks == animation_ticks,"Settled control retains its animation timer");
    DestroyWindow(h);
}
void exercise(HWND host, Surface& surface, const CardInk& ink, DWORD type) {
    const int width = ink.part == CardPart::dialog_close ? 40 : 240;
    const auto button = child(host, L"BUTTON", ink.part == CardPart::dialog_close ? L"Close" : L"Native caption",
                              WS_TABSTOP | type, 10);
    check(button != nullptr, "Cannot create test button");
    surface.card_style(button, ink);
    MoveWindow(button, 0, 0, width, 44, TRUE);
    SetFocus(button);
    const LPARAM inside = MAKELPARAM(10, 10), outside = MAKELPARAM(width + 20, 10);
    SendMessageW(button, WM_MOUSEMOVE, 0, inside);
    RedrawWindow(button, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    check_pixels(button, surface, ink, "rest");
    const int before = clicks;
    SendMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, inside);
    check(GetCapture() == button && (SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED), "Press lost capture");
    check_pixels(button, surface, ink, "mouse down");
    SendMessageW(button, WM_LBUTTONUP, 0, inside);
    check(clicks == before + 1, "Mouse activation count");
    if (type == BS_AUTOCHECKBOX)
        check(SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED, "Mouse click did not check toggle");
    check_pixels(button, surface, ink, "mouse up");
    SendMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, inside);
    SendMessageW(button, WM_MOUSEMOVE, MK_LBUTTON, outside);
    SendMessageW(button, WM_LBUTTONUP, 0, outside);
    check(clicks == before + 1 && GetCapture() != button, "Release outside activated the button");
    check_pixels(button, surface, ink, "cancelled press", false);
    SendMessageW(button, WM_KEYDOWN, VK_SPACE, 0);
    check_pixels(button, surface, ink, "Space down", false);
    SendMessageW(button, WM_KEYUP, VK_SPACE, 0);
    check(clicks == before + 2, "Space activation count");
    if (type == BS_AUTOCHECKBOX)
        check(SendMessageW(button, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "Space did not uncheck toggle");
    check_pixels(button, surface, ink, "Space up", false);
    DestroyWindow(button);
}
}
int main() {
    auto desktop = CreateDesktopW((L"ClipRope.ButtonPaint." + std::to_wstring(GetCurrentProcessId())).c_str(),
                                  nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) return 2;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClipRope.ButtonPaint";
    wc.lpfnWndProc = host_proc;
    if (!RegisterClassW(&wc)) return 2;
    HWND host = CreateWindowW(wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                              20, 20, 400, 230, nullptr, nullptr, wc.hInstance, nullptr);
    int result = 0;
    Surface surface;
    try {
        check(host != nullptr, "Cannot create test host");
        surface.attach(host);
#ifdef CLIPROPE_DESKTOP_PROBES
        using cliprope::application::Theme;
        SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&surface));
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        if (!(contrast.dwFlags & HCF_HIGHCONTRASTON)) {
            SetEnvironmentVariableW(L"CLIPROPE_PROBE_UI_THEME", L"dark");
            set_theme(Theme::light);
            check(!surface.colors.dark, "Explicit Light did not override Windows Dark");
            set_theme(Theme::system);
            check(surface.colors.dark, "System did not adopt Windows Dark");
            SetEnvironmentVariableW(L"CLIPROPE_PROBE_UI_THEME", L"light");
            SendMessageW(host, WM_SETTINGCHANGE, 0, 0);
            check(!surface.colors.dark, "Open surface missed the Windows theme change");
            set_theme(Theme::dark);
            SetEnvironmentVariableW(L"CLIPROPE_PROBE_UI_THEME", L"contrast");
            SendMessageW(host, WM_SETTINGCHANGE, 0, 0);
            check(surface.colors.contrast, "High contrast did not override Dark");
            SetEnvironmentVariableW(L"CLIPROPE_PROBE_UI_THEME", nullptr);
            set_theme(Theme::system);
        } else {
            check(surface.colors.contrast, "Windows high contrast is inactive in the open surface");
        }
#endif
        exercise(host, surface, {CardPart::dialog_close}, BS_PUSHBUTTON);
        exercise(host, surface, {CardPart::tray_header, 0xe71b, L"main", L"Current rope"}, BS_PUSHBUTTON);
        exercise(host, surface, {CardPart::tray_preview, 0xe8a5, L"Sample", {}, L"1."}, BS_PUSHBUTTON);
        exercise(host, surface, {CardPart::settings_nav, 0xe77f, L"Clipboard"}, BS_RADIOBUTTON);
        exercise(host, surface, {CardPart::toggle, 0xe8dd, L"Bold"}, BS_AUTOCHECKBOX);
        check_gdi_button(host, surface);
        check_color_states(host, surface);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    DestroyWindow(host);
    OleUninitialize();
    return result;
}
