#pragma once
#include "platform_win/tab_navigation.hpp"
#include "platform_win/card_press_input.hpp"
#include <memory>
#include <windows.h>
#include <mutex>
#include <thread>

namespace cliprope::win {
// The layered workspace passes clicks through transparent pixels. Route only
// wheel packets and navigation keys across its full visible row bands.
class RowInput final {
public:
    static constexpr UINT wheel_message = WM_APP + 0x431;
    static constexpr UINT shortcut_message = WM_APP + 0x432;
    using InstallHook = decltype(&SetWindowsHookExW);
    explicit RowInput(InstallHook install = SetWindowsHookExW) : install_(install) {}
    ~RowInput() { stop(); }
    // Returns the keyboard installation error. Wheel failure throws.
    DWORD start(HWND window);
    void configure(const ScopedKeys& keys);
    std::optional<DWORD> keyboard_status() const { return thread_.joinable() ? std::optional(keyboard_error_) : std::nullopt; }
    void suspend();
    void stop();
    void bounds(RECT screen);
    void navigation(bool allowed);
    bool accepts(LPARAM generation);
    std::shared_ptr<CardPressInput> track_card_press(HWND drag_input);
private:
    static LRESULT CALLBACK mouse_hook(int code, WPARAM kind, LPARAM data);
    static LRESULT CALLBACK keyboard_hook(int code, WPARAM kind, LPARAM data);
    bool covers(POINT screen);
    bool external_context();
    bool shortcut_scope();
    static thread_local RowInput* current_;
    InstallHook install_;
    HWND window_{};
    std::mutex mutex_;
    RECT bounds_{};
    bool enabled_{};
    bool navigation_{true};
    LPARAM generation_{};
    std::thread thread_;
    DWORD thread_id_{};
    DWORD keyboard_error_{};
    ScopedKeys bindings_ = default_scoped_keys;
    TabNavigation keys_;
    std::atomic<std::weak_ptr<CardPressInput>> card_press_;
};
}
