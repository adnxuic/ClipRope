#pragma once
#include <windows.h>
#include <atomic>

namespace cliprope::win {
// The workspace hooks observe both mouse and keyboard while a card press binds
// an Office selection. A release on the nonactivating card cannot move the Office caret.
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
    bool permits(DWORD first) const {
        LASTINPUTINFO latest{sizeof(latest)};
        return read_input_(&latest) && (first == latest.dwTime || last_pointer_.load() == latest.dwTime) &&
               !interrupted_.load();
    }
private:
    const HWND drag_input_;
    decltype(&GetLastInputInfo) read_input_;
    std::atomic<ULONGLONG> last_pointer_{~ULONGLONG{0}};
    std::atomic<bool> interrupted_{};
};
}
