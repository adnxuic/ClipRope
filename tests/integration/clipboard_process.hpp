#pragma once

#include <windows.h>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace probe {

inline std::wstring object_name(HANDLE object) {
    wchar_t name[256]{};
    DWORD needed{};
    return GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &needed) ? name : L"";
}

inline bool private_station() {
    wchar_t expected[256]{};
    const auto n = GetEnvironmentVariableW(L"CLIPROPE_PROBE_STATION", expected, 256);
    const auto name = object_name(GetProcessWindowStation());
    return n && n < 256 && !_wcsicmp(name.c_str(), expected) && _wcsicmp(name.c_str(), L"WinSta0");
}

inline std::wstring desktop_path() {
    return object_name(GetProcessWindowStation()) + L"\\" + object_name(GetThreadDesktop(GetCurrentThreadId()));
}

// A flushed child publication survives process exit on the service station.
// Remove only the fixture's exact synthetic text under the clipboard lock.
inline bool clear_private_sample(std::wstring_view expected) {
    if (!private_station() || !OpenClipboard(nullptr)) return false;
    const auto memory = GetClipboardData(CF_UNICODETEXT);
    const auto size = memory ? GlobalSize(memory) : 0;
    const auto text = size ? static_cast<const wchar_t*>(GlobalLock(memory)) : nullptr;
    const bool matches = text && size >= (expected.size() + 1) * sizeof(wchar_t) &&
                         std::wstring_view(text, expected.size()) == expected && !text[expected.size()];
    if (text) GlobalUnlock(memory);
    const bool cleared = matches && EmptyClipboard();
    CloseClipboard();
    return cleared;
}

// CommandLineToArgvW quoting, including a path ending in a backslash.
inline std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t backslashes{};
    for (wchar_t ch : argument) {
        if (ch == L'\\') { ++backslashes; continue; }
        result.append(ch == L'"' ? 2 * backslashes + 1 : backslashes, L'\\');
        result.push_back(ch);
        backslashes = 0;
    }
    result.append(2 * backslashes, L'\\');
    return result + L"\"";
}

inline HANDLE launch(const std::vector<std::wstring>& arguments, std::wstring desktop) {
    std::wstring command;
    for (const auto& arg : arguments) {
        if (!command.empty()) command += L' ';
        command += quote(arg);
    }
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = desktop.data();
    const auto duplicate = [](DWORD id) -> HANDLE {
        HANDLE copy{};
        auto original = GetStdHandle(id);
        if (original && original != INVALID_HANDLE_VALUE)
            DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS);
        return copy;
    };
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = duplicate(STD_INPUT_HANDLE);
    startup.hStdOutput = duplicate(STD_OUTPUT_HANDLE);
    startup.hStdError = duplicate(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    const bool created = CreateProcessW(arguments.front().c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &child) != FALSE;
    const auto error = GetLastError();
    for (auto handle : {startup.hStdInput, startup.hStdOutput, startup.hStdError}) if (handle) CloseHandle(handle);
    if (!created) {
        std::cerr << "CreateProcess error=" << error << '\n';
        return nullptr;
    }
    CloseHandle(child.hThread);
    return child.hProcess;
}

} // namespace probe
