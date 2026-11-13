#pragma once

#include "rendering/workspace_paint.hpp"

namespace cliprope::win {
// Both windows are transient. The input surface covers the virtual desktop
// during a hold. The smaller, click-through layered window carries the artwork.
class CardDrag {
  public:
    CardDrag() = default;
    ~CardDrag();
    CardDrag(const CardDrag &) = delete;
    CardDrag &operator=(const CardDrag &) = delete;
    void capture(HWND owner);
    void open_visual(HWND owner);
    void release_input();
    void close();
    HWND input() const { return input_; }
    void follow(POINT cursor, float angle);
    void place(presentation::PointF hook_pixels, UINT dpi, float angle);
    void refresh_face() { painted_dpi_ = 0; }
    void repaint_colors() {
        const auto dpi = painted_dpi_;
        refresh_face();
        if (visual_ && dpi) place(hook_, dpi, painted_angle_);
    }
    void retranslate(const localization::LocalizationManager& labels);
    presentation::PointF hook_pixels() const { return hook_; }

    const presentation::ColorPreview* colors{};
    application::CardSummary card;
    domain::Rope rope;
    std::size_t index{};
    presentation::ImageCardLayout geometry{};
    presentation::PointF grip;

  private:
    HWND input_{}, visual_{};
    HMONITOR monitor_{};
    UINT monitor_dpi_{}, painted_dpi_{};
    float painted_angle_{};
    presentation::PointF hook_;
    POINT origin_{};
};
} // namespace cliprope::win
