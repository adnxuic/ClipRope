#pragma once
#include <windows.h>

namespace cliprope::win {
// The native identity is enough to return keyboard focus. Only a paste gesture
// inspects UI Automation, Shell folders or Office selections.
struct MenuFocus {
    HWND foreground = GetForegroundWindow();
    DWORD pid{};
    DWORD thread = GetWindowThreadProcessId(foreground, &pid);
    GUITHREADINFO interrupted{sizeof(interrupted)};
    MenuFocus() { if (thread) GetGUIThreadInfo(thread, &interrupted); }

    void restore(HWND popup, HWND owner) const {
        DWORD current_pid{};
        if (GetForegroundWindow() != popup || !thread || (foreground != owner && pid == GetCurrentProcessId()) ||
            GetWindowThreadProcessId(foreground, &current_pid) != thread || current_pid != pid)
            return;
        const HWND child = interrupted.hwndFocus;
        const DWORD caller = GetCurrentThreadId();
        const bool join = thread != caller && child && IsChild(foreground, child) &&
            GetWindowThreadProcessId(child, nullptr) == thread && !IsHungAppWindow(foreground) &&
            AttachThreadInput(caller, thread, TRUE);
        SetForegroundWindow(foreground);
        if (join) {
            // Activation is synchronous on joined queues. Honor a field the receiver
            // has chosen, restoring our original child only in an otherwise empty frame.
            const HWND focus = GetFocus();
            if (GetForegroundWindow() == foreground && (!focus || focus == foreground) &&
                IsWindowEnabled(child) && IsWindowVisible(child))
                SetFocus(child);
            AttachThreadInput(caller, thread, FALSE);
        }
    }
};
}
