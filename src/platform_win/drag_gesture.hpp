#pragma once
#include <windows.h>
#include "presentation/image_card_layout.hpp"
#include <cstdlib>

namespace cliprope::win {
// Windows recognizes a double click by HWND and coordinates. A close press
// can now hit the next entry, so it must begin a fresh press/release pair.
constexpr bool begins_card_press(UINT message, bool close_control) {
    return message == WM_LBUTTONDOWN || (message == WM_LBUTTONDBLCLK && close_control);
}

// Direction of the grabbed edge in card coordinates. Corners carry both axes.
struct ResizeEdge {
    int x{}, y{};
    explicit operator bool() const { return x || y; }
    LPCWSTR cursor() const {
        if (x && y) return x == y ? IDC_SIZENWSE : IDC_SIZENESW;
        return x ? IDC_SIZEWE : IDC_SIZENS;
    }
};

inline ResizeEdge resize_edge(float x, float y, float width, float height, bool top) {
    if (x < 0 || x >= width || y < 0 || y >= height) return {};
    constexpr float band = 6;
    // A tiny image still needs a body for its native double-click and drag.
    const float side = std::min(band, width / 6), end = std::min(band, height / 6);
    const int horizontal = x < side ? -1 : x >= width - side ? 1 : 0;
    const int vertical = top && y < end ? -1 : y >= height - end ? 1 : 0;
    return {horizontal, vertical};
}

inline domain::CardScale dragged_scale(domain::CardScale start, ResizeEdge edge, presentation::SizeF base,
                                       float dx, float dy, bool image) {
    float dw = edge.x * dx / base.width, dh = edge.y * dy / base.height;
    if (image) {
        // Project corner motion onto the image diagonal. An edge uses its own axis.
        if (edge.x && edge.y)
            dw = (edge.x * dx * base.width + edge.y * dy * base.height) /
                 (base.width * base.width + base.height * base.height);
        else if (edge.y) dw = dh;
        dh = dw;
        start.height = start.width;
    }
    return {std::clamp(start.width + dw, domain::CardScale::minimum, domain::CardScale::maximum),
            std::clamp(start.height + dh, domain::CardScale::minimum, domain::CardScale::maximum)};
}

inline bool reached_drag_distance(POINT start, POINT point, UINT dpi) {
    return std::abs(point.x - start.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
           std::abs(point.y - start.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi);
}
}
