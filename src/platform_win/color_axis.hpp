#pragma once
#include "platform_win/fluent.hpp"
#include "domain/color.hpp"

namespace cliprope::win {
enum class ColorAxis { sv, hue, alpha };
HWND create_color_axis(HWND parent, int id, ColorAxis kind, UiText name,
                       std::function<void(double, double)> changed);
void update_color_axis(HWND control, domain::ColorHSV hsv, domain::ColorRGBA8 color);
} // namespace cliprope::win
