#pragma once
#include "platform_win/fluent.hpp"
#include "platform_win/menu_focus.hpp"
#include <optional>

namespace cliprope::win {
class FindPopup final {
public:
    ~FindPopup() {
        close(false);
    }
    void open(HWND owner, POINT anchor, UiText scope);
    void close(bool restore);
    bool translate(const MSG& msg);
    bool visible() const {
        return hwnd_ != nullptr;
    }
    void status(UiText count, UiText scope, bool navigable);
    std::function<void(std::wstring)> changed;
    std::function<void(int)> navigate;
    std::function<void()> closed;

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK edit_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    bool composing_{};
    void layout();
    void paint(HDC dc);
    HWND hwnd_{}, edit_{}, count_{}, scope_{}, prev_{}, next_{}, close_{};
    fluent::Surface surface_;
    std::optional<MenuFocus> focus_;
};
} // namespace cliprope::win
