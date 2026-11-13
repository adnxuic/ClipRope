#include "platform_win/app_icon.hpp"
#include "platform_win/tray_popup.hpp"
#include <shellapi.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace cliprope::win;
namespace {
void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
void dimensions(HICON icon, int size) {
    ICONINFO info{};
    require(GetIconInfo(icon, &info) != FALSE, "Icon handle is invalid");
    BITMAP bitmap{};
    const bool measured = GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    require(measured && bitmap.bmWidth == size && bitmap.bmHeight == size && bitmap.bmBitsPixel == 32,
            "Wrong native icon size or color depth");
}
#ifndef CLIPROPE_ICON_FALLBACK_TEST
HWND process_window(DWORD pid, const wchar_t* name) {
    HWND window{};
    while ((window = FindWindowExW(nullptr, window, name, nullptr))) {
        DWORD owner{};
        GetWindowThreadProcessId(window, &owner);
        if (owner == pid) return window;
    }
    return nullptr;
}
void resident(const wchar_t* binary, const wchar_t* process_id, bool close) {
    const auto pid = wcstoul(process_id, nullptr, 10);
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    require(process != nullptr, "Review process is unavailable");
    wchar_t actual[32768]{};
    DWORD length = DWORD(std::size(actual));
    const bool matches = QueryFullProcessImageNameW(process, 0, actual, &length) &&
                         std::filesystem::equivalent(actual, binary);
    require(matches, "Review PID must belong to the specified executable");
    const auto listener = process_window(pid, L"ClipRope.Listener");
    require(listener != nullptr, "Review listener not found");
    if (close) {
        PostMessageW(listener, WM_CLOSE, 0, 0);
        require(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "Review process did not exit cleanly");
        CloseHandle(process);
        std::cout << "Isolated review process exited cleanly\n";
        return;
    }
    PostMessageW(listener, WM_COMMAND, tray_command::settings, 0);
    HWND settings{};
    for (int attempt = 0; attempt != 100 && !settings; ++attempt) {
        Sleep(20);
        settings = process_window(pid, L"ClipRope.Settings");
    }
    for (auto window : {process_window(pid, L"ClipRope.Workspace"), settings}) {
        require(window != nullptr, "Production workspace/settings window missing");
        for (auto kind : {ICON_BIG, ICON_SMALL}) {
            DWORD_PTR result{};
            require(SendMessageTimeoutW(window, WM_GETICON, kind, 0, SMTO_ABORTIFHUNG, 2000, &result) != 0 && result,
                    "Production window did not publish its icon");
            const int metric = kind == ICON_BIG ? SM_CXICON : SM_CXSMICON;
            dimensions(reinterpret_cast<HICON>(result), GetSystemMetricsForDpi(metric, GetDpiForWindow(window)));
        }
    }
    const auto restart = RegisterWindowMessageW(L"TaskbarCreated");
    require(restart != 0 && SendMessageTimeoutW(listener, restart, 0, 0, SMTO_ABORTIFHUNG, 2000, nullptr) != 0,
            "Tray reinstall route did not respond");
    NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
    identifier.hWnd = listener;
    identifier.uID = 1;
    RECT bounds{};
    require(Shell_NotifyIconGetRect(&identifier, &bounds) == S_OK && !IsRectEmpty(&bounds),
            "Explorer did not retain the reinstalled notification icon");
    CloseHandle(process);
    std::cout << "Production workspace/settings native icons valid; Explorer notification icon retained after TaskbarCreated\n";
}

void executable_resource(const wchar_t* path) {
    const auto module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    require(module != nullptr, "Could not inspect the desktop executable");
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDI_CLIPROPE), RT_GROUP_ICON);
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto* header = loaded ? static_cast<const WORD*>(LockResource(loaded)) : nullptr;
    const bool ten_frames = header && SizeofResource(module, resource) >= 6 + 10 * 14 &&
                            header[0] == 0 && header[1] == 1 && header[2] == 10;
    FreeLibrary(module);
    require(ten_frames, "Desktop must embed the ten-frame application icon");
    for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        HICON extracted{};
        UINT id{};
        const auto count = PrivateExtractIconsW(path, 0, size, size, &extracted, &id, 1, 0);
        require(count == 1 && extracted, "Shell extraction failed");
        dimensions(extracted, size);
        DestroyIcon(extracted);
    }
}
#endif
} // namespace

int wmain(int argc, wchar_t** argv) try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#ifndef CLIPROPE_ICON_FALLBACK_TEST
    require(argc == 2 || argc == 4, "Pass the desktop executable, optionally --resident/--close and its review PID");
    executable_resource(argv[1]);
    if (argc == 4) {
        const bool close = std::wstring_view(argv[2]) == L"--close";
        require(close || std::wstring_view(argv[2]) == L"--resident", "Unknown resident review mode");
        resident(argv[1], argv[3], close);
        if (close) return 0;
    }
    const auto first = std::chrono::steady_clock::now();
    for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        const auto icon = application_icon(size);
        dimensions(icon, size);
        require(icon != LoadIconW(nullptr, IDI_APPLICATION), "Branded resource fell back to the system icon");
        require(application_icon(size) == icon, "Warm load did not reuse its handle");
    }
    const auto cold_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - first).count();
#else
    (void)argc; (void)argv;
    for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256})
        require(application_icon(size) == LoadIconW(nullptr, IDI_APPLICATION), "Missing resource must use the shared system fallback");
#endif
    const auto window = CreateWindowExW(0, L"STATIC", L"ClipRope icon fixture", WS_POPUP,
                                       0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr, "Fixture window creation failed");
    // Windows can lazily allocate a derived small icon on the first assignment.
    for (UINT dpi : {96, 120, 144, 192}) set_application_icons(window, dpi);
    const auto process = GetCurrentProcess();
    const auto users = GetGuiResources(process, GR_USEROBJECTS), gdi = GetGuiResources(process, GR_GDIOBJECTS);
    const auto repeated = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat != 100; ++repeat) {
        for (UINT dpi : {96, 120, 144, 192}) {
            set_application_icons(window, dpi);
            const auto large = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_BIG, 0));
            const auto small = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_SMALL, 0));
            require(large && small, "Window did not retain both icon handles");
#ifndef CLIPROPE_ICON_FALLBACK_TEST
            dimensions(large, GetSystemMetricsForDpi(SM_CXICON, dpi));
            dimensions(small, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
#endif
        }
    }
    require(users == GetGuiResources(process, GR_USEROBJECTS) && gdi == GetGuiResources(process, GR_GDIOBJECTS),
            "Repeated DPI updates leaked USER/GDI objects");
    const auto warm_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - repeated).count() / 400;
    set_application_icons(nullptr);
    DestroyWindow(window);
#ifndef CLIPROPE_ICON_FALLBACK_TEST
    std::cout << "Ten embedded/extracted RGBA sizes; four DPIs x 100 updates; stable USER/GDI; cold ten-size load " << cold_ms
              << " ms; warm update/inspection mean " << warm_ms << " ms\n";
#else
    std::cout << "Missing-resource fallback; four DPIs x 100 updates; valid shared handles and stable USER/GDI; warm update mean " << warm_ms << " ms\n";
#endif
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
