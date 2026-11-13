#include "platform_win/popup_menu.hpp"
#include "platform_win/row_input.hpp"
#include <future>
#include <stdexcept>

namespace cliprope::win {
thread_local RowInput* RowInput::current_{};

DWORD RowInput::start(HWND window) {
    if (thread_.joinable() && keyboard_error_) stop();
    if (thread_.joinable()) {
        std::lock_guard lock(mutex_);
        enabled_ = true;
        return ERROR_SUCCESS;
    }
    window_ = window;
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    std::promise<bool> started;
    auto ready = started.get_future();
    thread_ = std::thread([this, desktop, started = std::move(started)]() mutable {
        const bool attached = SetThreadDesktop(desktop) != FALSE;
        MSG msg{};
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        thread_id_ = GetCurrentThreadId();
        current_ = this;
        const auto mouse = attached ? install_(WH_MOUSE_LL, mouse_hook, GetModuleHandleW(nullptr), 0) : nullptr;
        keys_ = {};
        // Seed physical sides only, so releasing one side cannot leave a stale generic modifier.
        for (UINT key = 0; key < 256; ++key)
            if ((TabNavigation::modifier(key) || TabNavigation::bound(key, bindings_)) &&
                key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU)
                keys_.seed(key, (GetAsyncKeyState(key) & 0x8000) != 0);
        const auto keyboard = mouse ? install_(WH_KEYBOARD_LL, keyboard_hook, GetModuleHandleW(nullptr), 0) : nullptr;
        keyboard_error_ = keyboard ? ERROR_SUCCESS : GetLastError();
        if (!keyboard && !keyboard_error_) keyboard_error_ = ERROR_GEN_FAILURE;
        started.set_value(mouse != nullptr);
        if (mouse) {
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
            if (keyboard) UnhookWindowsHookEx(keyboard);
            UnhookWindowsHookEx(mouse);
        }
        current_ = nullptr;
    });
    if (!ready.get()) {
        thread_.join();
        throw std::runtime_error("Could not enable hovered-row scrolling");
    }
    std::lock_guard lock(mutex_);
    enabled_ = true;
    return keyboard_error_;
}
void RowInput::suspend() {
    if (auto press = card_press_.exchange({}).lock()) press->interrupt();
    {
        std::lock_guard lock(mutex_);
        enabled_ = false;
        ++generation_;
    }
    if (!window_) return;
    MSG pending{};
    while (PeekMessageW(&pending, window_, wheel_message, shortcut_message, PM_REMOVE)) {}
}
std::shared_ptr<CardPressInput> RowInput::track_card_press(HWND drag_input) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || keyboard_error_) return {};
    auto press = std::make_shared<CardPressInput>(drag_input);
    if (auto previous = card_press_.exchange(press).lock()) previous->interrupt();
    return press;
}
void RowInput::stop() {
    suspend();
    if (!thread_.joinable()) return;
    PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    thread_.join();
    MSG pending{};
    while (PeekMessageW(&pending, window_, wheel_message, shortcut_message, PM_REMOVE)) {}
}
void RowInput::bounds(RECT screen) {
    std::lock_guard lock(mutex_);
    bounds_ = screen;
}
void RowInput::configure(const ScopedKeys& keys) {
    std::lock_guard lock(mutex_);
    if (bindings_ == keys) return;
    ++generation_;
    for (auto chord : keys)
        if (chord.key && !TabNavigation::bound(chord.key, bindings_) && (GetAsyncKeyState(chord.key) & 0x8000))
            keys_.seed(chord.key, true);
    bindings_ = keys;
}
void RowInput::navigation(bool allowed) {
    std::lock_guard lock(mutex_);
    if (navigation_ && !allowed) ++generation_;
    navigation_ = allowed;
}
bool RowInput::accepts(LPARAM generation) {
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || !navigation_ || generation != generation_) return false;
    }
    return external_context();
}
bool RowInput::covers(POINT pt) {
    if (PopupMenuHost::input_active()) return false;
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || !PtInRect(&bounds_, pt)) return false;
    }
    if (!IsWindowVisible(window_) || IsIconic(window_)) return false;
    GUITHREADINFO gui{sizeof(gui)};
    if (!GetGUIThreadInfo(0, &gui) || gui.hwndCapture ||
        (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))) return false;
    const auto hit = WindowFromPoint(pt);
    if (hit == window_) return true;
    if (!hit || IsChild(window_, hit)) return false;
    // In a transparent gap the real recipient is below the workspace. An
    // editor, Floating card or other topmost window above it owns its wheel.
    for (auto w = GetAncestor(hit, GA_ROOT); w; w = GetWindow(w, GW_HWNDPREV))
        if (w == window_) return true;
    return false;
}
bool RowInput::external_context() {
    // Check both threads. A nonactivating drag or menu can belong to ClipRope
    // while the foreground editor still owns the keyboard.
    GUITHREADINFO foreground{sizeof(foreground)}, local{sizeof(local)};
    if (!GetGUIThreadInfo(0, &foreground) ||
        !GetGUIThreadInfo(GetWindowThreadProcessId(window_, nullptr), &local)) return false;
    constexpr DWORD menus = GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE;
    if (foreground.hwndCapture || local.hwndCapture || ((foreground.flags | local.flags) & menus)) return false;
    DWORD process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    // The workspace's local path owns its focused keys. Other ClipRope
    // windows keep dialog navigation and inline editing, wherever the mouse is.
    return process && process != GetCurrentProcessId();
}
bool RowInput::shortcut_scope() {
    if (PopupMenuHost::input_active()) return false;
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || !navigation_) return false;
    }
    POINT pt{};
    return external_context() && GetCursorPos(&pt) && covers(pt);
}
LRESULT CALLBACK RowInput::mouse_hook(int code, WPARAM kind, LPARAM data) {
    if (code == HC_ACTION && current_) {
        if (auto press = current_->card_press_.load().lock()) {
            const auto& packet = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            GUITHREADINFO gui{sizeof(gui)};
            if ((kind == WM_MOUSEMOVE || kind == WM_LBUTTONUP || kind == WM_LBUTTONDOWN) &&
                GetGUIThreadInfo(0, &gui) &&
                press->local_recipient(current_->window_, WindowFromPoint(packet.pt), gui.hwndCapture, kind))
                press->local_pointer(packet.time);
            else press->interrupt();
        }
    }
    if (code == HC_ACTION && kind == WM_MOUSEWHEEL && current_) {
        const auto& packet = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if (current_->covers(packet.pt)) {
            const auto ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000 ? MK_CONTROL : 0;
            std::lock_guard lock(current_->mutex_);
            if (current_->enabled_ && PostMessageW(current_->window_, wheel_message, MAKEWPARAM(ctrl, HIWORD(packet.mouseData)),
                             MAKELPARAM(packet.pt.x, packet.pt.y))) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, kind, data);
}
LRESULT CALLBACK RowInput::keyboard_hook(int code, WPARAM kind, LPARAM data) {
    if (code == HC_ACTION && current_) {
        auto& self = *current_;
        if (auto press = self.card_press_.load().lock()) press->interrupt();
        const auto& packet = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const bool down = kind == WM_KEYDOWN || kind == WM_SYSKEYDOWN;
        LPARAM generation{};
        ScopedKeys bindings;
        {
            std::lock_guard lock(self.mutex_);
            generation = self.generation_;
            bindings = self.bindings_;
        }
        const bool eligible = down && TabNavigation::bound(packet.vkCode, bindings) && self.shortcut_scope();
        std::lock_guard lock(self.mutex_);
        const auto route = self.keys_.key(packet.vkCode, down, eligible && generation == self.generation_, self.bindings_);
        if (route.command != WorkspaceShortcut::none &&
            !PostMessageW(self.window_, shortcut_message, static_cast<WPARAM>(route.command), generation)) {
            self.keys_.pass_press(packet.vkCode);
        } else if (route.consume) return 1;
    }
    return CallNextHookEx(nullptr, code, kind, data);
}
}
