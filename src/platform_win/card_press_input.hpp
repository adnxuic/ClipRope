#pragma once
#include <windows.h>
#include <atomic>
#include <optional>

namespace cliprope::win {
// The workspace hooks observe both mouse and keyboard while a card press binds
// an editor selection. A release on the nonactivating card leaves the original caret in place.
class CardPressInput {
public:
    explicit CardPressInput(HWND drag_input, decltype(&GetLastInputInfo) read_input = GetLastInputInfo)
        : drag_input_(drag_input), read_input_(read_input) {}
    bool local_recipient(HWND workspace, HWND hit, HWND capture, WPARAM message) const {
        if (hit == workspace) return !capture || capture == workspace;
        // CardDrag forwards moves and the first release from its owned capture window.
        return hit && hit == drag_input_ && (!capture || capture == hit) &&
               (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) && GetWindow(hit, GW_OWNER) == workspace;
    }
    void local_pointer(DWORD tick) { last_pointer_ = tick; }
    void interrupt() { interrupted_ = true; }
    bool permits(DWORD first) {
        LASTINPUTINFO latest{sizeof(latest)};
        if (!read_input_(&latest)) { interrupt(); return false; }
        if (interrupted_.load()) return false;
        const auto observed = last_pointer_.load();
        if (awaiting_tick_ && *awaiting_tick_ != latest.dwTime && observed != *awaiting_tick_) {
            interrupt();
            return false;
        }
        if (first == latest.dwTime || observed == latest.dwTime) {
            awaiting_tick_.reset();
            return !interrupted_.load();
        }
        // The session clock may advance before this event's low-level hook returns.
        // Remember that exact tick. A subsequent local event cannot excuse a gap.
        awaiting_tick_ = latest.dwTime;
        return false;
    }
    bool awaiting_hook() const { return awaiting_tick_.has_value() && !interrupted_.load(); }
private:
    const HWND drag_input_;
    decltype(&GetLastInputInfo) read_input_;
    std::atomic<ULONGLONG> last_pointer_{~ULONGLONG{0}};
    std::atomic<bool> interrupted_{};
    std::optional<DWORD> awaiting_tick_; // Only the binding thread reads or writes this value.
};
}
