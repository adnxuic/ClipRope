#pragma once
#include "rendering/screenshot_preview_service.hpp"
#include <windows.h>

namespace cliprope::win {
inline constexpr UINT screenshot_preview_ready = WM_APP + 248;
struct ScreenshotPlacement {
    RECT window{};
    unsigned width{}, height{}, padding{};
};
std::optional<ScreenshotPlacement> screenshot_placement(RECT card, RECT work, unsigned source_width,
                                                        unsigned source_height, UINT dpi);
std::uint64_t next_preview_owner();
// One UI-thread controller shared by all card windows. Call leave before destroying a surface.
class ScreenshotHover {
  public:
    ScreenshotHover(HWND owner, rendering::ScreenshotPreviewService::Resolve);
    ~ScreenshotHover();
    bool offer(HWND source, std::uint64_t generation, const application::CardSummary &, RECT anchor,
               std::function<bool()> still_hovered);
    void leave(HWND source);
    void cancel(bool purge = false);
    void enabled(bool);
    void ready();
    rendering::ScreenshotPreviewService::Counters counters() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cliprope::win
