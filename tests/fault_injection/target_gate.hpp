#pragma once
#include <windows.h>
#include <string>

namespace target_fixture {
// Only the qualification build on the fixture's private window station can
// model provider identity. An ordinary desktop can never synthesize this grant.
inline constexpr wchar_t selection[] = L"ClipRope.TargetFixture.Selected";
inline std::wstring gate() {
    wchar_t registered[256]{}, station[256]{}, point[48]{};
    DWORD bytes{};
    if (!GetEnvironmentVariableW(L"CLIPROPE_PROBE_STATION", registered, 256) ||
        !GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station, sizeof station, &bytes) ||
        _wcsicmp(station, registered) || !_wcsicmp(station, L"WinSta0") ||
        !GetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", point, 48)) return {};
    return point;
}
inline HWND receiver() {
    return gate().empty() ? nullptr : FindWindowW(L"ClipRope.TargetFixture", nullptr);
}
inline void signal_hold(const wchar_t* point) {
    if (gate() != point) return;
    const auto window = receiver();
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    const auto name = L"Local\\ClipRope.TargetHeld." + std::to_wstring(pid);
    if (auto handle = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())) {
        SetEvent(handle);
        CloseHandle(handle);
    }
    Sleep(5000);
}
} // namespace target_fixture
