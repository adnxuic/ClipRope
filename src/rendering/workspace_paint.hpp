#pragma once
#include "presentation/color_preview.hpp"

#include "presentation/viewport.hpp"
#include "application/card_search.hpp"
#include "application/appearance.hpp"
#include "localization/localization.hpp"
#include "presentation/hanging_layout.hpp"
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace cliprope::presentation {
class Swing;
}

namespace cliprope::rendering {
// Insets from the paper edges, in DIPs, shared with the native editing surface.
struct TextInset {
    static constexpr int side = 12, hanging_top = 11, floating_top = 44;
    static constexpr int hanging_bottom = 12, floating_bottom = 10;
};
inline unsigned paper_color(domain::ColorSelection background) { return domain::composite(background.resolve(domain::paper_default), domain::paper_default); }
struct AlphaArea {
    RECT rect;
    double opacity;
};
struct PaintedRow {
    const domain::Rope *rope;
    const presentation::Viewport *view;
    float top;
    presentation::InsertionPreview preview{};
};
// Transient pointer and animation state for one workspace frame.
struct Feedback {
    std::size_t rope_count = 1;
    std::string_view dragged, hovered, held, moving_rope, editing;
    float held_angle = 0;
    bool hover_close = false;
    const presentation::Swing *swing = nullptr;
    const application::CardSummary* edit_preview = nullptr;
    std::wstring_view find_term;
    const application::SearchHit* find_hit{};
    unsigned match_color = application::Appearance{}.match_rgb();
    const presentation::ColorPreview* colors{};
};
enum class FloatingControl { none, body, pencil, restore, close };
// A Floating sheet's Return pill starts 8 DIP from the left and 7 DIP from the
// top. Narrow sheets show only its arrow.
inline float return_control_right(float width) { return width >= 200 ? 84.f : 40.f; }

enum class FrameCoverage { partial, full };

// A full draw replaces all the pixels in bounds, including transparent ones.
// Draw into a premultiplied frame and present it. A layered HWND receives the
// frame's per-pixel alpha, scaled by each area's opacity. Other HWNDs get a copy.
bool paint_window(HWND window, HDC dc, RECT bounds, std::span<const AlphaArea> areas,
                  const std::function<bool(HDC)> &draw, POINT *screen_origin = nullptr,
                  FrameCoverage coverage = FrameCoverage::partial);
// Composite a frame over a caller's DC, as WM_PRINTCLIENT requires.
bool print_window(HDC dc, RECT bounds, const std::function<bool(HDC)> &draw);
void set_translucent(HWND window, bool enabled);
void trim_frame_after_hide();

// A failed presentation can leave the previous pixels on screen. Keep hit
// routing suspended until a whole frame succeeds, and schedule one repaint.
class FrameRecovery {
  public:
    bool failed() const { return failed_; }
    template<class Draw> bool present(HWND window, Draw draw) {
        try {
            if (draw()) {
                failed_ = false;
                return true;
            }
        } catch (...) {
            retry(window);
            throw;
        }
        retry(window);
        return false;
    }
  private:
    void retry(HWND window) {
        if (!failed_) InvalidateRect(window, nullptr, FALSE);
        failed_ = true;
    }
    bool failed_{};
};

#ifdef CLIPROPE_DRAW_PROBES
struct FrameBacking { LONG width{}, height{}; unsigned allocations{}; };
FrameBacking frame_backing();
enum class DrawFault { none, create_tile, finish_tile, finish_outer, present_layer };
// Thread-local and persistent until cleared, so a fixture can check bounded repaint.
void inject_draw_fault(DrawFault fault);
#endif

// Windows on the same UI thread reuse the DC render target, DWrite factory,
// brushes, text formats and artwork. Only the destination DC changes between paints.
class WorkspacePaint final {
  public:
    static std::shared_ptr<WorkspacePaint> acquire();
    static void use_localization(const localization::LocalizationManager* manager, std::wstring_view ui_family = L"Segoe UI");
    static std::size_t resident_bitmap_bytes();
    static std::size_t preview_bitmap_bytes();
    static std::size_t moving_face_bytes();
    static void discard_target();
    static std::uint64_t target_creations();
    // The same DPI-scaled fiber tile used by the paper under a native text field.
    HBRUSH editor_paper(HWND owner, UINT dpi, domain::ColorSelection background, std::string_view id, float x, float y, bool floating);
    void release_hanging();
    ~WorkspacePaint();
    bool draw(HDC dc, RECT bounds, UINT dpi, std::span<const PaintedRow> rows, const Feedback &feedback);
    bool draw_carried(HDC dc, RECT bounds, UINT dpi, const application::CardSummary &card,
                      const domain::Rope &rope, std::size_t index, const presentation::ImageCardLayout& geometry,
                      presentation::CardFrame frame, const presentation::ColorPreview* colors = nullptr);
    bool draw_floating(HDC dc, RECT bounds, UINT dpi, const application::CardSummary &card,
                       FloatingControl hover = FloatingControl::none, bool editing = false, const presentation::ColorPreview* colors = nullptr);

  private:
    WorkspacePaint();
    struct Device;
    std::unique_ptr<Device> device_;
};
} // namespace cliprope::rendering
