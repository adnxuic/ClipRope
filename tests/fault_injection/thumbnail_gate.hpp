#pragma once
#include <windows.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace thumbnail_fixture {
inline constexpr wchar_t stage_variable[] = L"CLIPROPE_PROBE_THUMBNAIL_STAGE";
inline thread_local DWORD owner_pid{};
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

inline std::wstring event_name(DWORD pid, const wchar_t* suffix) {
    return L"Local\\ClipRope.ThumbnailGate." + std::to_wstring(pid) + suffix;
}

inline void report_discard() {
    Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name(GetCurrentProcessId(), L".discarded").c_str()), CloseHandle);
    if (event) SetEvent(event.get());
}

inline void pause_at(std::wstring_view stage) {
    wchar_t selected[32]{};
    const auto size = GetEnvironmentVariableW(stage_variable, selected, 32);
    if (!size || size >= 32 || std::wstring_view(selected, size) != stage) return;
    const auto pid = owner_pid ? owner_pid : GetCurrentProcessId();
    Handle entered(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name(pid, L".entered").c_str()), CloseHandle);
    Handle release(OpenEventW(SYNCHRONIZE, FALSE, event_name(pid, L".release").c_str()), CloseHandle);
    if (!entered || !release || !SetEvent(entered.get()) || WaitForSingleObject(release.get(), 15000) != WAIT_OBJECT_0)
        throw std::runtime_error("Thumbnail fixture gate unavailable");
}
}
