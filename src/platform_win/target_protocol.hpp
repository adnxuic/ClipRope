#pragma once
#include "platform_win/delivery.hpp"
#include <cstdint>

namespace cliprope::win::target {
inline constexpr DWORD signature = 0x31544754;
inline constexpr DWORD bind_ms = 500, delivery_ms = 1500;
enum class Kind : DWORD { none, uia, office, folder, warm };
enum class Verdict : DWORD { refused, accepted, lost, replaced };

// Each side writes only its own phase, then signals the corresponding event.
// No provider-owned pointer, profile name, clipboard bytes or folder path crosses IPC.
struct Channel {
    DWORD magic{signature}, size{sizeof(Channel)};
    std::uint64_t serial{}, foreground{}, field{}, bind_deadline{};
    DWORD pid{}, tid{}, input_tick{}, warm{}, guarded_office_press{};
    std::uint64_t bound_serial{}, bound_at{};
    Kind kind{};
    char description[96]{};
    DWORD payload{}, sequence{};
    std::uint64_t verify_deadline{}, reply_serial{}, replied_at{};
    Verdict verdict{};
};
inline bool valid_focus(const Channel& c) {
    HWND root = reinterpret_cast<HWND>(c.foreground), field = reinterpret_cast<HWND>(c.field);
    GUITHREADINFO info{sizeof(info)};
    DWORD pid{};
    return GetForegroundWindow() == root && GetGUIThreadInfo(0, &info) &&
           !(info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) &&
           info.hwndActive == root && info.hwndFocus == field && GetAncestor(field, GA_ROOT) == root &&
           GetWindowThreadProcessId(field, &pid) == c.tid && pid == c.pid &&
           IsWindowVisible(field) && IsWindowEnabled(field);
}
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
} // namespace cliprope::win::target
