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
inline constexpr wchar_t changed_input[] = L"ClipRope.TargetFixture.InputChanged";
inline constexpr wchar_t next_tick[] = L"ClipRope.TargetFixture.NextTick";
inline bool synchronized_press() {
    const auto stage = gate();
    return stage == L"uia-before" || stage == L"uia-reading";
}
inline std::wstring press_event(HWND field, const wchar_t* phase) {
    DWORD pid{};
    GetWindowThreadProcessId(field, &pid);
    return L"Local\\ClipRope.Press." + std::to_wstring(pid) + L"." + phase;
}
inline bool release_at(HWND field, const wchar_t* stage, DWORD initial_tick) {
    if (gate() != stage) return true;
    if (!SetPropW(field, next_tick, reinterpret_cast<HANDLE>(UINT_PTR(initial_tick) + 1))) return false;
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, press_event(field, L"ready").c_str());
    HANDLE resume = OpenEventW(SYNCHRONIZE, FALSE, press_event(field, L"resume").c_str());
    const bool ok = ready && resume && SetEvent(ready) && WaitForSingleObject(resume, 450) == WAIT_OBJECT_0;
    if (resume) CloseHandle(resume);
    if (ready) CloseHandle(ready);
    return ok;
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
