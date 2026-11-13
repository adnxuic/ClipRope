#include "platform_win/find_popup.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void capture(HWND window, const std::filesystem::path& destination) {
    RECT r{};
    GetClientRect(window, &r);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), r.right, -r.bottom, 1, 32, BI_RGB};
    HDC dc = CreateCompatibleDC(nullptr);
    void* pixels{};
    auto image = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    require(dc && image, "Cannot allocate Find appearance frame");
    auto old = SelectObject(dc, image);
    require(PrintWindow(window, dc, PW_CLIENTONLY) != FALSE, "Cannot print Find popup");
    GdiFlush();
    const auto palette = cliprope::win::fluent::system_palette();
    require(GetPixel(dc, cliprope::win::fluent::px(window, 315), cliprope::win::fluent::px(window, 54)) ==
                palette.panel,
            "Find popup did not use the current theme");
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + r.right * r.bottom * 4;
    std::ofstream out(destination, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    out.write(static_cast<const char*>(pixels), r.right * r.bottom * 4);
    require(out.good(), "Cannot save Find appearance frame");
    SelectObject(dc, old);
    DeleteObject(image);
    DeleteDC(dc);
}
void run(const wchar_t* file, int dpi) {
    using namespace cliprope::win;
    HWND owner = CreateWindowExW(0, L"STATIC", L"Fixture", WS_POPUP, 0, 0, 320, 160, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    require(owner != nullptr, "Fixture owner unavailable");
    FindPopup popup;
    popup.open(owner, {20, 20}, message(TextKey::find_scope_all,{{"rope",L"材料"}}));
    auto hwnd = FindWindowW(L"ClipRope.FindInCards", nullptr);
    require(hwnd && GetWindow(hwnd, GW_OWNER) == owner, "Fixture popup unavailable");
    auto edit = GetDlgItem(hwnd, 1);
    SetWindowTextW(edit, L"磁性");
    popup.status(L"3 / 12", message(TextKey::find_scope_all,{{"rope",L"材料"}}), true);
    RECT r{};
    GetClientRect(hwnd, &r);
    require(r.right == MulDiv(372, dpi, 96) && r.bottom == MulDiv(76, dpi, 96), "Unexpected popup DPI size");
    for (int id : {1, 4, 5, 6}) {
        RECT b{};
        GetWindowRect(GetDlgItem(hwnd, id), &b);
        require(b.bottom - b.top == MulDiv(id == 1 ? 36 : 30, dpi, 96), "Wrong input or navigation hit height");
    }
    capture(hwnd, file);
    popup.close(false);
    DestroyWindow(owner);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    cliprope::localization::LocalizationManager manager;
    manager.set_language(cliprope::localization::Language::automatic,cliprope::win::preferred_ui_locale());
    cliprope::win::UiLocalization language(manager);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    auto name = L"ClipRope.FindAppearance." + std::to_wstring(GetCurrentProcessId());
    auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 1;
    int result = 0;
    try {
        run(argv[1], _wtoi(argv[2]));
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    SetThreadDesktop(original);
    CloseDesktop(desktop);
    return result;
}
