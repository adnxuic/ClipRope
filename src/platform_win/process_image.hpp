#pragma once
#include <windows.h>
#include <string>

namespace cliprope::win {
inline DWORD window_process(HWND window) {
    DWORD pid{};
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid;
}

inline std::wstring process_basename(DWORD pid) {
    if (!pid) return {};
    std::wstring path(32768, L'\0');
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    DWORD size = static_cast<DWORD>(path.size());
    const bool available = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
    CloseHandle(process);
    if (!available) return {};
    path.resize(size);
    return path.substr(path.find_last_of(L"\\/") + 1);
}
}
