#pragma once
#include "platform_win/fluent.hpp"

namespace cliprope::win {
// A premultiplied-alpha underlay supplies the silhouette and shadow. The foreground HWND
// contains native controls and is clipped entirely inside the opaque face of the underlay.
class CardMenuFrame final {
  public:
    static int shadow_margin(UINT dpi) { return int((22 * dpi + 95) / 96); }
    static constexpr size_t cache_budget = 8u * 1024 * 1024;
    static unsigned generations();
    static size_t cache_bytes();
    bool open(HWND owner);
    bool fit(HWND surface, const fluent::Palette &theme, UINT dpi, float corner = 15.f);
    HWND window() const { return window_; }
    void close();
    ~CardMenuFrame();

  private:
    HWND window_{};
};
} // namespace cliprope::win
