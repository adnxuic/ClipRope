#pragma once
#include "presentation/color_preview.hpp"
#include "platform_win/inline_editor_host.hpp"
#include "presentation/viewport.hpp"
#include "platform_win/screenshot_hover.hpp"
#include <windows.h>
#include <functional>
#include <memory>

namespace cliprope::win {

// Call on the PerMonitorV2 UI thread. Coordinates in Layout are relative to
// the selected monitor's work area in DIPs.
domain::Layout floating_layout_at(POINT screen_point, float grab_x = 0.5f, float grab_y = 0.25f,
                                  const application::CardSummary* card = nullptr);
RECT resolve_floating(const domain::Layout &layout, bool adaptive = false);
struct FloatingMonitor {
    RECT work;
    std::string name;
    UINT dpi;
    domain::Layout centered_text_layout() const;
};
domain::Layout centered_text_layout(POINT pointer);
std::vector<domain::Layout> tile_file_group(HWND source, std::vector<RECT> occupied, std::size_t count,
                                            domain::ImageSizeMode image_size, bool includes_images);

struct FloatingActions {
    const presentation::ColorPreview* colors{};
    ScreenshotHover* screenshot_hover{};
    std::function<void(const domain::Entry&, domain::CardScale, const domain::Layout*)> resize;
    InlineEditorHost* editor{};
    std::function<void(const domain::Id&)> edit;
    std::function<void(application::CardSummary)> external_drag;
    std::function<bool()> visible_overview;
    std::function<void(const domain::Id &, const presentation::RopeSlot &)> copy_to_rope;
    std::function<void(const domain::Id &)> press, double_click, return_current, close_card;
    std::function<std::optional<presentation::RopeSlot>(POINT, const application::CardSummary&)> preview_at;
    std::function<void()> request_preview;
    std::function<void()> clear_preview;
    std::function<void(const domain::Id &, const presentation::RopeSlot &)> drop;
    std::function<void(const domain::Id &, const domain::Layout &)> moved;
    std::function<void(application::CardMenuSnapshot, POINT)> context;
    InlineEditorHost::Save save_text;
    std::function<void()> error;
    std::function<void()> cancel_paste;
    std::function<void(HWND)> lost_focus;
};

class Floating final {
  public:
    explicit Floating(FloatingActions actions);
    ~Floating();
    Floating(const Floating &) = delete;
    Floating &operator=(const Floating &) = delete;
    bool open(application::CardSummary card, const domain::Layout &layout, bool show_window = true);
    void show();
    void update(application::CardSummary card);
    void edit_text(application::TextDraft draft);
    HWND window() const;
    std::uint64_t presented_frames() const;
    bool shows_file_pixels(std::uint64_t version = 0) const;
    std::string_view preview_key() const;
    // Re-evaluate the grip and slot number when the workspace moves under a captured pointer.
    static void refresh_drag_preview(HWND capture, POINT screen);
    domain::Layout layout() const;
    std::uint64_t gesture_epoch() const;
    void restore_placement(const domain::Layout&);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cliprope::win
