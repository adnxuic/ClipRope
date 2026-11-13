#include "clipboard_process.hpp"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    // A standard user's service station receives a logon-derived name.
    // Serialize callers across build directories and CTest instances.
    HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\ClipRopeProbeStation");
    if (!lock) return 1;
    const auto wait = WaitForSingleObject(lock, 60000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return 1;
    const auto original = GetProcessWindowStation();
    constexpr DWORD access = WINSTA_READATTRIBUTES | WINSTA_CREATEDESKTOP | WINSTA_ACCESSCLIPBOARD |
                             WINSTA_ACCESSGLOBALATOMS | WINSTA_ENUMDESKTOPS;
    auto station = CreateWindowStationW(nullptr, CWF_CREATE_ONLY, access, nullptr);
    if (!station && GetLastError() == ERROR_ALREADY_EXISTS)
        station = CreateWindowStationW(nullptr, 0, access, nullptr);
    if (!station) {
        std::cerr << "Private station unavailable error=" << GetLastError() << '\n';
        return 1;
    }
    const auto name = probe::object_name(station);
    if (name.empty() || !_wcsicmp(name.c_str(), L"WinSta0") ||
        !SetEnvironmentVariableW(L"CLIPROPE_PROBE_STATION", name.c_str()) || !SetProcessWindowStation(station)) {
        std::cerr << "Private station selection failed error=" << GetLastError() << '\n';
        return 1;
    }
    const auto desktop_name = L"ClipRopeProbe-" + std::to_wstring(GetCurrentProcessId());
    const auto original_desktop = GetThreadDesktop(GetCurrentThreadId());
    const auto desk = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desk || !SetThreadDesktop(desk) || !OpenClipboard(nullptr)) {
        std::cerr << "Private desktop unavailable error=" << GetLastError() << '\n';
        return 1;
    }
    // The system may already have created the noninteractive service station.
    // Never take over existing contents. Query only presence while locked.
    SetLastError(ERROR_SUCCESS);
    const bool empty = EnumClipboardFormats(0) == 0 && GetLastError() == ERROR_SUCCESS && !GetClipboardOwner();
    CloseClipboard();
    if (!empty || !SetProcessWindowStation(original) || !SetThreadDesktop(original_desktop)) {
        std::cerr << "Noninteractive clipboard not empty or station transition failed\n";
        return 1;
    }
    HANDLE child = probe::launch({argv + 1, argv + argc}, name + L"\\" + desktop_name);
    DWORD result = 1;
    if (child) {
        WaitForSingleObject(child, INFINITE);
        GetExitCodeProcess(child, &result);
        CloseHandle(child);
    }
    CloseDesktop(desk);
    CloseWindowStation(station);
    ReleaseMutex(lock);
    CloseHandle(lock);
    return static_cast<int>(result);
}
