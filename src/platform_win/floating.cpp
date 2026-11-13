#include "platform_win/menu_timing.hpp"
#include "platform_win/floating.hpp"
#include "presentation/card_labels.hpp"
#include "platform_win/card_drag.hpp"
#include "platform_win/drag_gesture.hpp"
#include "rendering/workspace_paint.hpp"
#include "platform_win/image_tip.hpp"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace cliprope::win {
namespace {
constexpr auto floating_class = L"ClipRope.Floating";
constexpr UINT drag_preview_changed = WM_APP + 0x433;
enum class Part { none, body, pencil, restore, close, resize };
FloatingMonitor monitor_info(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT dx{}, dy{};
    if (!GetMonitorInfoW(monitor, &info) || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy)) ||
        !dx)
        throw std::runtime_error("Floating monitor is unavailable");
    std::wstring_view name(info.szDevice);
    const int size = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), utf8.data(), size, nullptr,
                        nullptr);
    return {info.rcWork, std::move(utf8), dx};
}
struct Search {
    std::string_view name;
    HMONITOR found{};
};
BOOL CALLBACK locate(HMONITOR h, HDC, LPRECT, LPARAM data) {
    auto &search = *reinterpret_cast<Search *>(data);
    try {
        if (monitor_info(h).name == search.name) {
            search.found = h;
            return FALSE;
        }
    } catch (const std::exception &) {
    }
    return TRUE;
}
double dip(LONG px, UINT dpi) { return double(px) * 96 / dpi; }
int px(double n, UINT dpi) { return static_cast<int>(std::lround(n * dpi / 96)); }
double finite(double n, double fallback) { return std::isfinite(n) ? n : fallback; }
domain::Layout relative(RECT rect) {
    const auto m = monitor_info(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST));
    return {m.name, dip(rect.left - m.work.left, m.dpi), dip(rect.top - m.work.top, m.dpi),
            dip(rect.right - rect.left, m.dpi), dip(rect.bottom - rect.top, m.dpi)};
}
} // namespace

domain::Layout floating_layout_at(POINT pt, float grab_x, float grab_y, const application::CardSummary* card) {
    const auto m = monitor_info(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    const auto g = presentation::ImageCardLayout::measure(card ? *card : application::CardSummary{});
    const auto [w, h] = g.floating_size(card ? card->scale : domain::CardScale{});
    const float grip_x = g.adaptive ? (w - g.body_width) / 2 + g.body_width * grab_x : w * grab_x;
    const float grip_y = g.adaptive ? g.toolbar + g.height() * grab_y : h * grab_y;
    domain::Layout layout{m.name, dip(pt.x - m.work.left, m.dpi) - grip_x,
                          dip(pt.y - m.work.top, m.dpi) - grip_y, w, h};
    return relative(resolve_floating(layout, g.adaptive));
}
domain::Layout FloatingMonitor::centered_text_layout() const {
    domain::Layout layout{name};
    const double w = dip(work.right - work.left, dpi), h = dip(work.bottom - work.top, dpi);
    layout.width = std::min(layout.width, w);
    layout.height = std::min(layout.height, h);
    layout.x = (w - layout.width) / 2;
    layout.y = (h - layout.height) / 2;
    return layout;
}
domain::Layout centered_text_layout(POINT pointer) {
    return monitor_info(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST)).centered_text_layout();
}

std::vector<domain::Layout> tile_file_group(HWND source, std::vector<RECT> occupied, std::size_t count,
                                            domain::ImageSizeMode image_size, bool includes_images) {
    const auto monitor = monitor_info(MonitorFromWindow(source, MONITOR_DEFAULTTONEAREST));
    const RECT work = monitor.work;
    // Reserve room for adaptive previews before the background decoder knows their aspect ratios.
    const auto image = presentation::image_limit(image_size);
    const LONG width = std::min<LONG>(work.right - work.left,
        px(includes_images ? std::max(240.f, image.width) : 240.f, monitor.dpi));
    const LONG height = std::min<LONG>(work.bottom - work.top,
        px(includes_images ? std::max(160.f, image.height + presentation::ImageCardLayout::toolbar) : 160.f, monitor.dpi));
    const LONG gap = px(8, monitor.dpi);
    std::vector<domain::Layout> result;
    auto add = [&](LONG x, LONG y) {
        result.push_back({monitor.name, dip(x - work.left, monitor.dpi), dip(y - work.top, monitor.dpi),
                          std::min(240., dip(width, monitor.dpi)), std::min(160., dip(height, monitor.dpi))});
    };
    for (LONG y = work.top; y + height <= work.bottom && result.size() < count;) {
        LONG next_row = work.bottom;
        for (LONG x = work.left; x + width <= work.right && result.size() < count;) {
            RECT slot{x, y, x + width, y + height}, intersection{};
            LONG next = x;
            for (const auto& other : occupied) {
                if (!IntersectRect(&intersection, &slot, &other)) continue;
                next = std::max(next, other.right + gap);
                next_row = std::min(next_row, other.bottom + gap);
            }
            if (next == x) {
                add(x, y);
                occupied.push_back(slot);
                next = slot.right + gap;
                next_row = std::min(next_row, slot.bottom + gap);
            }
            x = next;
        }
        y = next_row;
    }
    const LONG offset = px(24, monitor.dpi);
    const LONG columns = 1 + (work.right - work.left - width) / offset;
    const LONG rows = 1 + (work.bottom - work.top - height) / offset;
    for (std::size_t i = 1; result.size() < count; ++i)
        add(work.left + LONG(i % columns) * offset, work.top + LONG(i % rows) * offset);
    return result;
}

RECT resolve_floating(const domain::Layout &layout, bool adaptive) {
    Search search{layout.monitor};
    if (!search.name.empty())
        EnumDisplayMonitors(nullptr, nullptr, locate, reinterpret_cast<LPARAM>(&search));
    auto m = monitor_info(search.found ? search.found : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const auto w = std::max<LONG>(1, m.work.right - m.work.left),
               h = std::max<LONG>(1, m.work.bottom - m.work.top);
    const auto width = std::min(dip(w, m.dpi), std::max(144., finite(layout.width, 240)));
    const auto height = std::min(dip(h, m.dpi), std::max(adaptive ? 38. : 80., finite(layout.height, 160)));
    const auto x = std::clamp(finite(layout.x, 0), 0., dip(w, m.dpi) - width);
    const auto y = std::clamp(finite(layout.y, 0), 0., dip(h, m.dpi) - height);
    const POINT origin{m.work.left + px(x, m.dpi), m.work.top + px(y, m.dpi)};
    return {origin.x, origin.y, origin.x + px(width, m.dpi), origin.y + px(height, m.dpi)};
}

struct Floating::Impl {
    FloatingActions actions;
    application::CardSummary card;
    HWND hwnd{};
    RECT original{};
    std::uint64_t gesture_epoch{};
    std::optional<domain::Layout> pending_origin;
    POINT start{};
    std::optional<presentation::RopeSlot> hanging;
    std::unique_ptr<CardDrag> copy_preview;
    presentation::PointF grip{};
    UINT original_dpi = 96, drag_dpi = 96;
    HMONITOR drag_monitor{};
    ResizeEdge edge;
    domain::CardScale original_scale;
    Part pressed = Part::none;
    rendering::FloatingControl hovered = rendering::FloatingControl::none;
    bool refresh_after_release = false;
    bool armed = false, dragging = false, polling = false, escape = false, tracking = false;
    rendering::FrameRecovery frames;
    std::uint64_t presentations{};
    std::shared_ptr<rendering::WorkspacePaint> paint;
    bool editing() const { return hwnd && actions.editor && actions.editor->owner() == hwnd; }
    ImageTip image_tip;
    const std::uint64_t preview_owner = next_preview_owner();
    explicit Impl(FloatingActions callbacks) : actions(std::move(callbacks)) {}
    ~Impl() {
        if (actions.screenshot_hover) actions.screenshot_hover->leave(hwnd);
        if (editing()) actions.editor->discard();
        if (hwnd)
            DestroyWindow(hwnd);
    }
    RECT rect() const {
        RECT r{};
        GetWindowRect(hwnd, &r);
        return r;
    }
    ResizeEdge border(POINT pt) const {
        RECT r{};
        GetClientRect(hwnd, &r);
        const auto dpi = GetDpiForWindow(hwnd);
        const float x = float(dip(pt.x, dpi)), y = float(dip(pt.y, dpi));
        const float w = float(dip(r.right, dpi)), h = float(dip(r.bottom, dpi));
        const auto g = presentation::ImageCardLayout::measure(card);
        if (g.adaptive && y >= g.toolbar) {
            const auto image = g.fitted_image(w, h);
            return resize_edge(x - (w - image.width) / 2, y - g.toolbar, image.width, image.height, false);
        }
        return resize_edge(x, y, w, h, true);
    }
    Part hit(POINT pt) const {
        if (frames.failed()) return Part::none;
        auto dpi = GetDpiForWindow(hwnd);
        RECT r{};
        GetClientRect(hwnd, &r);
        if (!PtInRect(&r, pt))
            return Part::none;
        const double x = dip(pt.x, dpi), y = dip(pt.y, dpi), w = dip(r.right, dpi);
        if (y >= 6 && y < 32 && x >= 8 && x < rendering::return_control_right(float(w)))
            return Part::restore;
        if (y >= 6 && y < 32 && x >= w - 32 && x < w - 6)
            return Part::close;
        if (!editing() && tracking && card.menu == application::CardMenuKind::text &&
            y >= 6 && y < 32 && x >= w - 59 && x < w - 32) return Part::pencil;
        if (actions.resize && border(pt) && !(editing() && actions.editor->previewing())) return Part::resize;
        return Part::body;
    }
    RECT fitted(domain::Layout layout) const {
        const auto g = presentation::ImageCardLayout::measure(card);
        if (g.adaptive || card.scale != domain::CardScale{}) {
            const auto size = g.floating_size(card.scale);
            layout.width = size.width;
            layout.height = size.height;
        }
        const auto available = resolve_floating(layout, g.adaptive);
        if (!g.adaptive) return available;
        const auto fitted = relative(available);
        const auto image = g.fitted_image(float(fitted.width), float(fitted.height));
        layout.width = std::max(144.f, image.width);
        layout.height = g.toolbar + image.height;
        return resolve_floating(layout, true);
    }
    bool layered() const { return presentation::adaptive_image(card) || card.style.opacity < 1; }
    void place(RECT r) {
        if (!SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                          SWP_NOACTIVATE | SWP_NOZORDER))
            throw std::runtime_error("Could not position Floating card");
    }
    void release() {
        KillTimer(hwnd, 1);
        polling = false;
        if (escape) {
            UnregisterHotKey(hwnd, 1);
            escape = false;
        }
        pressed = Part::none;
        dragging = false;
        if (refresh_after_release && actions.request_preview) actions.request_preview();
        refresh_after_release = false;
        copy_preview.reset();
        if (GetCapture() == hwnd)
            ReleaseCapture();
    }
    void leave_rope() {
        if (!hanging) return;
        hanging.reset();
        if (actions.clear_preview) actions.clear_preview();
        rendering::set_translucent(hwnd, layered());
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void cancel() {
        if (actions.cancel_paste) actions.cancel_paste();
        const bool restore = dragging && !copy_preview;
        if (pressed == Part::resize) card.scale = original_scale;
        armed = false;
        release();
        leave_rope();
        if (restore) {
            place(original);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
    bool tiny_edge(Part part) const {
        if (part != Part::resize) return false;
        const auto g = presentation::ImageCardLayout::measure(card);
        return g.adaptive && std::min(g.body_width, g.height()) < 12;
    }
    void down(POINT pt, bool ctrl) {
        ++gesture_epoch;
        image_tip.hide();
        cancel();
        pressed = hit(pt);
        const bool tiny = tiny_edge(pressed);
        if (tiny && ctrl) pressed = Part::body;
        if (editing() && pressed == Part::body) {
            pressed = Part::none;
            actions.editor->focus();
            return;
        }
        if (pressed == Part::none)
            return;
        start = pt;
        ClientToScreen(hwnd, &start);
        original = rect();
        if (pressed == Part::resize) {
            edge = border(pt);
            original_scale = card.scale;
        }
        original_dpi = drag_dpi = GetDpiForWindow(hwnd);
        drag_monitor = MonitorFromPoint(start, MONITOR_DEFAULTTONEAREST);
        grip = {float(pt.x) / std::max(1L, original.right - original.left),
                float(pt.y) / std::max(1L, original.bottom - original.top)};
        armed = pressed == Part::body || tiny;
        if (armed && actions.press)
            actions.press(card.entry.id);
        SetCapture(hwnd);
        if (GetCapture() != hwnd) { cancel(); return; }
        polling = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (polling)
            SetTimer(hwnd, 1, 16, nullptr);
    }
    void move(POINT screen, bool ctrl, bool released = false) {
        if (pressed == Part::resize) {
            if (!dragging && !reached_drag_distance(start, screen, original_dpi)) return;
            if (!dragging && polling) escape = RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
            if (armed && actions.cancel_paste) actions.cancel_paste();
            armed = false;
            dragging = true;
            const auto g = presentation::ImageCardLayout::measure(card);
            const presentation::SizeF base = g.adaptive
                ? presentation::SizeF{g.body_width / card.scale.width, g.height() / card.scale.width}
                : presentation::SizeF{240, 160};
            auto start_scale = original_scale;
            if (!g.adaptive && original_scale == domain::CardScale{}) {
                // A profile from before border resizing can have a custom window rectangle.
                start_scale.width = float(dip(original.right - original.left, original_dpi)) / base.width;
                start_scale.height = float(dip(original.bottom - original.top, original_dpi)) / base.height;
            }
            card.scale = dragged_scale(start_scale, edge, base, float(dip(screen.x - start.x, original_dpi)),
                                       float(dip(screen.y - start.y, original_dpi)), g.adaptive);
            const auto geometry = presentation::ImageCardLayout::measure(card);
            const auto size = geometry.floating_size(card.scale);
            const auto work = monitor_info(drag_monitor).work;
            // The stationary edge bounds expansion near the work-area perimeter.
            const LONG available_w = edge.x < 0 ? original.right - work.left : work.right - original.left;
            const LONG available_h = edge.y < 0 ? original.bottom - work.top : work.bottom - original.top;
            LONG w = std::clamp<LONG>(px(size.width, original_dpi), 1, std::max(1L, available_w));
            LONG h = std::clamp<LONG>(px(size.height, original_dpi), 1, std::max(1L, available_h));
            if (g.adaptive) {
                const auto image = geometry.fitted_image(float(dip(w, original_dpi)), float(dip(h, original_dpi)));
                w = std::min<LONG>(w, px(std::max(144.f, image.width), original_dpi));
                h = std::min<LONG>(h, px(g.toolbar + image.height, original_dpi));
            }
            const auto left = std::clamp(edge.x < 0 ? original.right - w : original.left, work.left, work.right - w);
            const auto top = std::clamp(edge.y < 0 ? original.bottom - h : original.top, work.top, work.bottom - h);
            const RECT next{left, top, left + w, top + h};
            const auto current = rect();
            if (!EqualRect(&current, &next)) {
                place(next);
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            return;
        }
        if (pressed != Part::body)
            return;
        if (!dragging && !reached_drag_distance(start, screen, original_dpi))
            return;
        if (!dragging && ctrl) {
            if (!actions.visible_overview || !actions.visible_overview()) {
                cancel();
                if (!released && actions.external_drag) actions.external_drag(card);
                return;
            }
            copy_preview = std::make_unique<CardDrag>();
            copy_preview->card = card;
            copy_preview->geometry = presentation::ImageCardLayout::measure(
                card, float(dip(original.right - original.left, original_dpi)) / card.scale.width);
            copy_preview->open_visual(hwnd);
        }
        if (copy_preview && !actions.visible_overview()) {
            cancel();
            return;
        }
        if (!dragging && polling)
            escape = RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
        if (!dragging && actions.cancel_paste) actions.cancel_paste();
        dragging = true;
        pending_origin.reset();
        armed = false;
        const auto monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
        if (monitor != drag_monitor) {
            drag_dpi = monitor_info(monitor).dpi;
            drag_monitor = monitor;
        }
        const auto target = actions.preview_at ? actions.preview_at(screen, card) : std::nullopt;
        const auto source = presentation::ImageCardLayout::measure(card);
        const auto g = target ? target->geometry : source;
        // The original Floating toolbar and minimum width do not shrink with
        // the target rope. Map the original image grip into its new body.
        const float local_x = (grip.x - 0.5f) *
            (source.adaptive ? source.float_width() * g.body_width / source.body_width : g.width);
        const float local_y = g.top + (source.adaptive ?
            (grip.y * (source.toolbar + source.height()) - source.toolbar) * g.height() / source.height() :
            grip.y * g.height());
        if (copy_preview) {
            if (target && (!hanging || target->rope.id != hanging->rope.id || target->index != hanging->index ||
                           target->geometry != hanging->geometry || target->rope.capacity != hanging->rope.capacity)) {
                copy_preview->rope = target->rope;
                copy_preview->index = target->index;
                copy_preview->geometry = target->geometry;
                copy_preview->refresh_face();
            }
            if (copy_preview->geometry != g) {
                copy_preview->geometry = g;
                copy_preview->refresh_face();
            }
            if (!target) leave_rope();
            hanging = target;
            copy_preview->grip = {local_x, local_y};
            copy_preview->follow(screen, 0);
            return;
        }
        if (target) {
            const bool redraw = !hanging || hanging->index != target->index || hanging->rope.id != target->rope.id ||
                                hanging->rope.capacity != target->rope.capacity || hanging->geometry != target->geometry;
            hanging = target;
            rendering::set_translucent(hwnd, true);
            const auto w = target->geometry.width;
            const float pad = g.carried_padding();
            const auto left = screen.x - px(pad + w / 2 + local_x, drag_dpi);
            const auto top = screen.y - px(24 + local_y, drag_dpi);
            const RECT next{left, top, left + px(w + 2 * pad, drag_dpi), top + px(g.carried_height(), drag_dpi)};
            const auto current = rect();
            if ((redraw || !EqualRect(&next, &current)) &&
                !SetWindowPos(hwnd, HWND_TOPMOST, left, top, next.right - left, next.bottom - top, SWP_NOACTIVATE))
                throw std::runtime_error("Could not place the Floating hanger preview");
            if (redraw)
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        } else {
            const bool changed = hanging.has_value();
            leave_rope();
            const auto w = MulDiv(original.right - original.left, drag_dpi, original_dpi);
            const auto h = MulDiv(original.bottom - original.top, drag_dpi, original_dpi);
            const auto left = screen.x - std::lround(grip.x * w), top = screen.y - std::lround(grip.y * h);
            const RECT next{left, top, left + w, top + h};
            const auto current = rect();
            if (!EqualRect(&next, &current)) place(next);
            if (changed)
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
    void up(POINT screen, bool ctrl) {
        if (pressed == Part::none)
            return;
        move(screen, ctrl, true);
        if (pressed == Part::resize) {
            if (dragging && card.scale != original_scale) {
                const auto layout = relative(rect());
                actions.resize(card.entry, card.scale, &layout);
                pending_origin.reset();
            }
            release();
            return;
        }
        POINT client = screen;
        ScreenToClient(hwnd, &client);
        const auto action = pressed;
        const bool moved = dragging;
        const bool copy = bool(copy_preview);
        const auto target = hanging;
        release();
        if (copy) {
            leave_rope();
            if (target && actions.copy_to_rope) actions.copy_to_rope(card.entry.id, *target);
        } else if (moved) {
            try {
                if (target && actions.drop) {
                    actions.drop(card.entry.id, *target);
                    leave_rope();
                } else {
                    leave_rope();
                    place(fitted(relative(rect())));
                    if (actions.moved)
                        actions.moved(card.entry.id, relative(rect()));
                }
            } catch (...) {
                leave_rope();
                place(original);
                throw;
            }
        } else if (action == hit(client)) {
            if (action == Part::pencil && actions.edit) actions.edit(card.entry.id);
            if (action == Part::restore && actions.return_current)
                actions.return_current(card.entry.id);
            if (action == Part::close && actions.close_card)
                actions.close_card(card.entry.id);
        }
    }
    void sync_editor() {
        if (!editing()) return;
        RECT area{};
        GetClientRect(hwnd, &area);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&area), 2);
        const auto dpi = GetDpiForWindow(hwnd);
        area.left += px(rendering::TextInset::side, dpi);
        area.right -= px(rendering::TextInset::side, dpi);
        area.top += px(rendering::TextInset::floating_top, dpi);
        area.bottom -= px(rendering::TextInset::floating_bottom, dpi);
        actions.editor->place(area, area, true);
    }
    void edit_text(application::TextDraft draft) {
        if (!actions.editor || draft.entry_id != card.entry.id) return;
        cancel();
        actions.editor->bind(hwnd, std::move(draft), card, actions.save_text, [this] {
            HDC dc = GetDC(hwnd);
            struct Release { HWND hwnd; HDC dc; ~Release() { if (dc) ReleaseDC(hwnd, dc); } } release{hwnd, dc};
            return dc && present(dc);
        }, [this](const application::CardSummary* saved) {
            if (saved) card.accept_text_edit(*saved);
            InvalidateRect(hwnd, nullptr, FALSE);
        });
        sync_editor();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    bool draw(HDC dc, rendering::FloatingControl hover) {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        if (!paint)
            paint = rendering::WorkspacePaint::acquire();
        if (hanging && !copy_preview)
            return paint->draw_carried(dc, bounds, GetDpiForWindow(hwnd), card, hanging->rope, hanging->index,
                                       hanging->geometry, {hanging->geometry.width / 2 + hanging->geometry.carried_padding(), 24, 0},
                                       actions.colors);
        const auto* preview = editing() ? actions.editor->preview() : nullptr;
        return paint->draw_floating(dc, bounds, GetDpiForWindow(hwnd), preview ? *preview : card, hover,
                                    editing() && !actions.editor->previewing(), actions.colors);
    }
    bool screenshot_body(const domain::Entry& entry) const {
        if (!hwnd || !IsWindowVisible(hwnd) || pressed != Part::none || dragging || hanging || copy_preview || editing() ||
            card.entry.id != entry.id || card.entry.snapshot_id != entry.snapshot_id || card.entry.revision != entry.revision) return false;
        POINT point{};
        if (!GetCursorPos(&point) || WindowFromPoint(point)!=hwnd || !ScreenToClient(hwnd,&point) || hit(point)!=Part::body) return false;
        RECT r{}; GetClientRect(hwnd,&r);
        const auto dpi=GetDpiForWindow(hwnd);
        const auto g=presentation::ImageCardLayout::measure(card);
        const auto picture=g.fitted_image(float(dip(r.right,dpi)),float(dip(r.bottom,dpi)));
        const auto x=dip(point.x,dpi), y=dip(point.y,dpi), left=(dip(r.right,dpi)-picture.width)/2;
        return y>=g.toolbar && y<g.toolbar+picture.height && x>=left && x<left+picture.width;
    }
    void hover(POINT pt) {
        if (!tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&track) != FALSE;
        }
        const auto part = hit(pt);
        bool full=false;
        if(actions.screenshot_hover) {
            if(screenshot_body(card.entry)) full=actions.screenshot_hover->offer(hwnd,preview_owner,card,rect(),
                [this,entry=card.entry] { return screenshot_body(entry); });
            else actions.screenshot_hover->leave(hwnd);
        }
        image_tip.update(hwnd, part == Part::body && !full ? &card : nullptr);
        const auto control = part == Part::restore ? rendering::FloatingControl::restore
                             : part == Part::close ? rendering::FloatingControl::close
                             : part == Part::pencil ? rendering::FloatingControl::pencil
                             : part == Part::body ? rendering::FloatingControl::body : rendering::FloatingControl::none;
        if (control == hovered)
            return;
        hovered = control;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void unhover() {
        if(actions.screenshot_hover) actions.screenshot_hover->leave(hwnd);
        image_tip.hide();
        tracking = false;
        if (hovered == rendering::FloatingControl::none)
            return;
        hovered = rendering::FloatingControl::none;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    bool present(HDC dc) {
        const bool shown = frames.present(hwnd, [this, dc] { return present_frame(dc); });
#ifdef CLIPROPE_DRAW_PROBES
        if (shown) ++presentations;
#endif
        return shown;
    }
    bool present_frame(HDC dc) {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        if (hanging && !copy_preview)
            return rendering::paint_window(hwnd, dc, bounds, {}, [&](HDC dest) { return draw(dest, hovered); });
        auto area = bounds;
        if (card.style.opacity == 0)
            area.top = px(38, GetDpiForWindow(hwnd));
        const rendering::AlphaArea patches[] = {
            {area, editing() && !actions.editor->previewing() && card.style.opacity == 0 ? 1. : card.style.opacity},
            {editing() ? actions.editor->inset(hwnd) : RECT{}, 0}
        };
        return rendering::paint_window(hwnd, dc, bounds, patches, [&](HDC dest) { return draw(dest, hovered); });
    }
    LRESULT message(UINT msg, WPARAM w, LPARAM l) {
        if(actions.screenshot_hover && (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK || msg==WM_RBUTTONDOWN ||
            msg==WM_MBUTTONDOWN || msg==WM_MOUSEWHEEL || msg==WM_CONTEXTMENU || msg==WM_ENTERMENULOOP ||
            msg==WM_DPICHANGED || msg==WM_MOVE || msg==WM_SIZE || msg==WM_SHOWWINDOW || msg==WM_NCDESTROY))
            actions.screenshot_hover->leave(hwnd);
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && actions.lost_focus) actions.lost_focus(hwnd);
            break;
        case WM_SETCURSOR: {
            POINT p{};
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (pressed == Part::resize || hit(p) == Part::resize) {
                SetCursor(LoadCursorW(nullptr, (pressed == Part::resize ? edge : border(p)).cursor()));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            try {
                if (dc) present(dc);
            } catch (...) {
                EndPaint(hwnd, &ps);
                throw;
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            rendering::print_window(reinterpret_cast<HDC>(w), bounds, [&](HDC dest) {
                return draw(dest, rendering::FloatingControl::none);
            });
            return 0;
        }
        case WM_SHOWWINDOW:
            if (!w) rendering::trim_frame_after_hide();
            break;
        case WM_MOUSEWHEEL:
            if (editing()) { actions.editor->wheel(w); return 0; }
            break;
        case drag_preview_changed:
            if (dragging && pressed == Part::body && GetCapture() == hwnd)
                move({static_cast<LONG>(w), static_cast<LONG>(l)}, bool(copy_preview));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const auto part = msg == WM_LBUTTONDBLCLK ? hit(pt) : Part::none;
            if (begins_card_press(msg, part == Part::close)) {
                down(pt, (w & MK_CONTROL) != 0);
            } else if (armed && !(w & MK_CONTROL) && (part == Part::body || tiny_edge(part)) && actions.double_click) {
                armed = false;
                actions.double_click(card.entry.id);
            } else if (part == Part::resize || ((w & MK_CONTROL) && part == Part::body)) {
                down(pt, (w & MK_CONTROL) != 0);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (w & MK_LBUTTON) {
                ClientToScreen(hwnd, &pt);
                move(pt, (w & MK_CONTROL) != 0);
            } else
                hover(pt);
            return 0;
        case WM_MOUSELEAVE:
            unhover();
            return 0;
        case WM_LBUTTONUP:
            ClientToScreen(hwnd, &pt);
            up(pt, (w & MK_CONTROL) != 0);
            return 0;
        case WM_TIMER:
            if (w == 1 && polling) {
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    cancel();
                    return 0;
                }
                if (GetCursorPos(&pt)) {
                    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
                        move(pt, ctrl);
                    else
                        up(pt, ctrl);
                }
            }
            return 0;
        case WM_CANCELMODE:
            cancel();
            return 0;
        case WM_CAPTURECHANGED:
            if (pressed != Part::none)
                cancel();
            return 0;
        case WM_HOTKEY:
            if (w == 1 && escape)
                cancel();
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                cancel();
                return 0;
            }
            break;
        case WM_CONTEXTMENU:
            menu_mark(MenuStage::input);
            if (!editing() && actions.context)
                actions.context(application::CardMenuSnapshot(card), pt);
            return 0;
        case WM_DPICHANGED: {
            if (pressed == Part::resize) cancel();
            const auto suggested = *reinterpret_cast<const RECT*>(l);
            place(dragging ? suggested : fitted(relative(suggested)));
            if (actions.request_preview) actions.request_preview();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOVE: case WM_SIZE:
            sync_editor();
            break;
        case WM_DISPLAYCHANGE:
            cancel();
            place(fitted(relative(rect())));
            return 0;
        case language_changed:
            refresh_ui_bindings(hwnd);
            presentation::localize_card(card, ui_localization());
            image_tip.hide();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            return 0; // Only an explicit card close changes history.
        case WM_NCDESTROY: {
            auto old = hwnd;
            hwnd = nullptr;
            SetWindowLongPtrW(old, GWLP_USERDATA, 0);
            return DefWindowProcW(old, msg, w, l);
        }
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        try {
            return self->message(m, w, l);
        } catch (const std::exception &) {
            if (self->pressed != Part::none) self->cancel();
            if (self->actions.error)
                self->actions.error();
            return 0;
        }
    }
};
Floating::Floating(FloatingActions actions) : impl_(std::make_unique<Impl>(std::move(actions))) {}
Floating::~Floating() = default;
void Floating::refresh_drag_preview(HWND capture, POINT screen) {
    SendMessageW(capture, drag_preview_changed, screen.x, screen.y);
}
bool Floating::open(application::CardSummary card, const domain::Layout &layout, bool show_window) {
    if (impl_->hwnd)
        return false;
    WNDCLASSW cls{};
    cls.style = CS_DBLCLKS;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = Impl::proc;
    cls.lpszClassName = floating_class;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    impl_->card = std::move(card);
    if (presentation::adaptive_image(impl_->card) && !impl_->card.source.width)
        impl_->pending_origin = layout;
    const auto r = impl_->fitted(layout);
    if (!create_ui_window(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, floating_class,
                         tr(TextKey::card_floating_title), WS_POPUP | WS_CLIPCHILDREN, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         nullptr, nullptr, cls.hInstance, impl_.get()))
        return false;
    rendering::set_translucent(impl_->hwnd, impl_->layered());
    if (show_window)
        show();
    return true;
}
void Floating::show() {
    ShowWindow(impl_->hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(impl_->hwnd);
}
void Floating::update(application::CardSummary card) {
    if (card.entry.id != impl_->card.entry.id)
        throw std::invalid_argument("Floating card identity changed");
    if (impl_->pressed != Part::none) { impl_->refresh_after_release = true; return; }
    const bool was_image = presentation::adaptive_image(impl_->card);
    impl_->card = std::move(card);
    rendering::set_translucent(impl_->hwnd, impl_->hanging.has_value() || impl_->layered());
    const bool is_image = presentation::adaptive_image(impl_->card);
    if (!impl_->dragging && (is_image || was_image)) {
        auto current = impl_->rect();
        auto layout = impl_->pending_origin.value_or(relative(current));
        if (!is_image) {
            const auto size = presentation::ImageCardLayout::measure(impl_->card).floating_size(impl_->card.scale);
            layout.width = size.width;
            layout.height = size.height;
            impl_->image_tip.hide();
        }
        auto next = impl_->fitted(layout);
        if (!is_image || impl_->card.source.width) impl_->pending_origin.reset();
        if (!EqualRect(&current, &next)) impl_->place(next);
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
void Floating::edit_text(application::TextDraft draft) { impl_->edit_text(std::move(draft)); }
HWND Floating::window() const { return impl_->hwnd; }
std::uint64_t Floating::presented_frames() const { return impl_->presentations; }
std::string_view Floating::preview_key() const { return impl_->card.preview_key(); }
bool Floating::shows_file_pixels(std::uint64_t version) const {
    return !impl_->card.file_list_hash.empty() && impl_->card.thumbnail &&
           (!version || impl_->card.thumbnail->source.version == version);
}
domain::Layout Floating::layout() const { return relative(impl_->rect()); }
std::uint64_t Floating::gesture_epoch() const { return impl_->gesture_epoch; }
void Floating::restore_placement(const domain::Layout& layout) { impl_->place(impl_->fitted(layout)); }
} // namespace cliprope::win
