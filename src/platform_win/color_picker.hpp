#pragma once
#include "platform_win/fluent.hpp"
#include "platform_win/defer_history_command.hpp"
#include "presentation/color_picker.hpp"

namespace cliprope::win {
struct ColorPickerOptions {
    domain::ColorSelection value;
    unsigned default_color = domain::ink_default;
    bool allow_alpha{};
    std::vector<unsigned> presets{0x786956, 0xa66e3d, 0xd9b58b, 0xe5dac7, 0x7b9d91, 0x7c94b2, 0xa985aa};
    std::optional<unsigned> contrast_background, contrast_ink;
    bool large_text{}, translucent{};
    presentation::ColorPickerCallbacks callbacks;
    DeferHistoryCommand defer_history;
};
class ColorPickerPopup {
  public:
    static bool track(HWND owner, HWND anchor, ColorPickerOptions options);
    static bool active();
    static bool owns(HWND window);
    static void dismiss();
};
enum class ColorButtonStyle { labeled, compact, settings };
void color_picker_button(fluent::Surface &surface, HWND button, domain::ColorSelection color,
                         unsigned fallback, ColorButtonStyle style = ColorButtonStyle::labeled, bool alpha = false);
} // namespace cliprope::win
