#pragma once

#include <windows.h>
#include <functional>

namespace cliprope::win {

struct OverlayGestures {
    std::function<void()> on_press;
    std::function<void()> on_double_click;
};

// Created and destroyed on the owning per-monitor-aware UI thread.
class Overlay {
public:
    explicit Overlay(OverlayGestures actions = {});
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool open(POINT host_origin);
    HWND window() const { return window_; }
    RECT card_screen_rect() const;
    bool set_right_slot(bool right);
    bool move_to(POINT host_origin);

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    RECT card_rect() const;
    bool update_region();
    OverlayGestures actions_;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    bool right_ = false;
    bool dragging_ = false;
    POINT pressed_at_{};
    POINT offset_dip_{};
};

} // namespace cliprope::win
