#include "history_observer.hpp"
#include "resident.hpp"
#include <iostream>

namespace {
using namespace cliprope;
using probe::require;

HWND child(HWND parent, int id) {
    struct Find { int id; HWND hwnd{}; } result{id};
    EnumChildWindows(parent, [](HWND hwnd, LPARAM data) -> BOOL {
        auto& target = *reinterpret_cast<Find*>(data);
        if (GetDlgCtrlID(hwnd) != target.id) return TRUE;
        target.hwnd = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&result));
    return result.hwnd;
}
std::wstring caption(HWND hwnd) {
    wchar_t value[256]{};
    SendMessageW(hwnd, WM_GETTEXT, std::size(value), reinterpret_cast<LPARAM>(value));
    return value;
}
void run(const wchar_t* exe) {
    history_fixture::Directory dir;
    probe::Resident desktop(exe, dir.path, false);
    auto observer = history_fixture::observe(dir.path, SQLITE_OPEN_READWRITE);
    SendMessageW(desktop.listener(), WM_COMMAND, 30, 0);
    const auto settings = probe::owned_window(desktop.pid(), L"ClipRope.Settings");
    require(settings, "Settings unavailable");
    SendMessageW(child(settings, 1504), BM_CLICK, 0, 0);
    const auto launcher = child(settings, 1903);
    require(launcher, "Settings color launcher unavailable");
    const auto original = caption(launcher);
    persistence::exec(observer.get(), "CREATE TRIGGER reject_recent BEFORE INSERT ON recent_colors "
                                      "BEGIN SELECT RAISE(ABORT,'fixture'); END");
    PostMessageW(launcher, BM_CLICK, 0, 0);
    HWND picker{}, hex{}, error{};
    probe::pump_until([&] {
        picker = probe::owned_window(desktop.pid(), L"ClipRope.ColorPicker");
        if (!picker) return false;
        hex = child(picker, 4150);
        error = child(picker, 4133);
        return hex && error && child(picker, IDOK);
    }, "Settings color editor unavailable");
    SendMessageW(hex, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"#123456"));
    PostMessageW(child(picker, IDOK), BM_CLICK, 0, 0);
    probe::pump_until([&] { return !caption(error).empty(); }, "Save failure was not displayed");
    require(caption(launcher) == original && caption(hex) == L"#123456", "Failed Apply changed the displayed color or lost the draft");
    persistence::exec(observer.get(), "DROP TRIGGER reject_recent");
    PostMessageW(child(picker, IDOK), BM_CLICK, 0, 0);
    probe::pump_until([&] { return !IsWindow(picker) && caption(launcher).find(L"#123456") != std::wstring::npos; },
                      "Retried Apply did not update the displayed color");
    SendMessageW(settings, WM_CLOSE, 0, 0);
    desktop.exit();
    std::cout << "Settings color preserved after failed Apply and updated by retry\n";
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !probe::private_station()) return 2;
    try { run(argv[1]); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
