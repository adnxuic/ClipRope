#include "platform_win/popup_menu.hpp"
#include "platform_win/menu_timing.hpp"
#include "platform_win/workspace.hpp"
#include "platform_win/app_icon.hpp"
#include "platform_win/card_drag.hpp"
#include "platform_win/floating.hpp"
#include "platform_win/image_tip.hpp"
#include "platform_win/drag_gesture.hpp"
#include "platform_win/row_input.hpp"
#include "platform_win/rope_placement.hpp"
#include "presentation/hanging_layout.hpp"
#include "presentation/overview.hpp"
#include "presentation/swing.hpp"
#include "rendering/workspace_paint.hpp"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cliprope::win {
namespace {
constexpr auto class_name = L"ClipRope.Workspace";
constexpr UINT_PTR swing_timer = 2;
constexpr UINT_PTR rope_edge_timer = 3;
constexpr UINT_PTR card_edge_timer = 4;
enum class Gesture { idle, pending, resizing, carrying, landing, rope_pending, rope_drag };
using presentation::Hanging;
enum class Part { gap, paper, pencil, erase, select, resize };
struct Hit {
    Part part = Part::gap;
    std::string id;
    std::string rope;
    ResizeEdge edge;
};

void unite(HRGN region, HRGN piece) {
    if (!piece)
        throw std::runtime_error("Unable to construct workspace hit region");
    const auto result = CombineRgn(region, region, piece, RGN_OR);
    DeleteObject(piece);
    if (result == ERROR)
        throw std::runtime_error("Unable to combine workspace hit regions");
}
} // namespace

struct Workspace::Impl {
    WorkspaceReads reads;
    presentation::Viewport view;
    presentation::Overview list;
    WorkspaceActions actions;
    HWND hwnd{};
    UINT dpi = 96;
    float width = 0, height = 0;
    bool topmost = true;
    domain::ImagePreferences image_preferences;
    std::optional<application::CardSummary> floating_source;
    HWND floating_capture{};
    domain::Id edge_rope, edge_waiting_for_editor;
    bool edge_scrolled{}, browsed_during_drag{};
    std::chrono::steady_clock::time_point last_edge_tick;
    unsigned match_color = application::Appearance{}.match_rgb();
    std::wstring find_term;
    std::optional<application::SearchHit> find_hit;
    float row_height() const { return list.row_height; }
    presentation::RopeCurve curve;
    presentation::Swing swing;
    std::chrono::steady_clock::time_point last_frame;
    std::vector<float> installed_shape;
    std::uint64_t paints=0;
    rendering::FrameRecovery frames;
    bool shown = false, escape_registered = false, overview = false,
         sizing = false, animating = false, hover_close = false, layout_pending = false;
    domain::Rope current;
    std::size_t rope_count = 1;
    bool menu_rebin{}, menu_multiple{};
    std::string rope, first_card, first_rope, drop_rope, hovered;
    Hit pressed;
    Gesture gesture = Gesture::idle;
    std::uint64_t gesture_epoch{};
    CardDrag drag;
    struct Resize {
        presentation::Viewport original;
        domain::Entry entry;
        domain::CardScale start, current;
        presentation::SizeF base;
        presentation::CardFrame frame;
        POINT screen;
        std::size_t index;
        bool image;
    };
    std::optional<Resize> resize;
    ScopedKeys scoped_keys = default_scoped_keys;
    RowInput row_input;
    RECT work_area{}, start_bounds{};
    POINT rope_down{};
    // Keep the carried row drawable when edge scrolling recycles its original slot.
    std::optional<presentation::RopeRow> carried_rope;
    float rope_grip_y{}, carried_top{};
    std::string previous_first, reorder_target;
    std::size_t reorder_from{}, reorder_to{};
    int wheel_remainder{};
    ImageTip image_tip;
    const std::uint64_t preview_owner = next_preview_owner();
    POINT down{}, cursor{};
    bool physical_press = false;
    float held_angle{};
    std::size_t source_index{};
    float source_advance{};
    bool copying = false, pending_previews = false;
    presentation::PointF landing_start;
    double landing_time{};
    std::optional<std::size_t> drop;
    std::shared_ptr<rendering::WorkspacePaint> paint;
    domain::Id editing_rope;
    bool editing() const { return actions.editor && hwnd && actions.editor->owner() == hwnd; }
    bool any_editor() const { return actions.editor && actions.editor->owner(); }

    Impl(WorkspaceReads source, WorkspaceActions callbacks)
        : reads(std::move(source)), view(reads), list(reads), actions(std::move(callbacks)) {}
    ~Impl() {
        if (actions.screenshot_hover) actions.screenshot_hover->leave(hwnd);
        if (hwnd) {
            row_input.stop();
            if (editing()) actions.editor->discard();
            cancel();
            DestroyWindow(hwnd);
        }
    }
    int px(float dip) const { return static_cast<int>(std::lround(dip * dpi / 96)); }
    float dip(int px_value) const { return float(px_value) * 96 / dpi; }
    void resized(float w, float h) {
        width = w;
        height = h;
        curve = presentation::RopeCurve(w);
    }
    // Cards within the swing allowance of the edges are still drawn.
    bool near_view(float x, float paper) const { return x + paper + 48 > 0 && x - 48 < width; }
    RECT card_rect(const presentation::Viewport &v, std::size_t original, float top,
                   const presentation::InsertionPreview& preview = {}, std::optional<std::size_t> projected = {}) const {
        const auto g = v.geometry(original);
        const float cx = v.projected_left(projected.value_or(original), preview) + g.width / 2;
        const float r = top + curve.contact(cx);
        return {px(cx - g.body_width / 2), px(r + g.top), px(cx + g.body_width / 2), px(r + g.bottom)};
    }
    // A layered HWND takes its whole alpha map from UpdateLayeredWindow, so a
    // frame can be presented outside WM_PAINT and before the window is shown.
    bool present() {
        return frames.present(hwnd, [this] { return present_frame(); });
    }
    bool present_frame() {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        if (!shown || bounds.right <= 0 || bounds.bottom <= 0)
            return true;
        if (!paint)
            paint = rendering::WorkspacePaint::acquire();
        sync_editor();
        const auto rows = scene();
        const auto state = feedback();
        const rendering::AlphaArea cut{editing() ? actions.editor->inset(hwnd) : RECT{}, 0};
        const bool done = rendering::paint_window(hwnd, nullptr, bounds, {&cut, 1}, [&](HDC dest) {
            return paint->draw(dest, bounds, dpi, rows, state);
        }, nullptr, rendering::FrameCoverage::full);
        if (done)
            ++paints;
        if (done && layout_pending) {
            layout_pending = false;
            if (actions.layout_changed)
                actions.layout_changed();
        }
        return done;
    }
    rendering::Feedback feedback() const {
        rendering::Feedback result;
        result.colors = actions.colors;
        result.rope_count = rope_count;
        result.match_color = match_color;
        result.find_term = find_term;
        result.find_hit = find_hit ? &*find_hit : nullptr;
        if (overview && gesture == Gesture::rope_drag) result.moving_rope = pressed.rope;
        if ((gesture == Gesture::carrying && !copying) || gesture == Gesture::landing)
            result.dragged = drag.card.entry.id;
        if (gesture == Gesture::pending || gesture == Gesture::resizing) {
            result.held = pressed.id;
            result.held_angle = held_angle;
        }
        result.hovered = hovered;
        result.hover_close = hover_close;
        if (editing()) {
            result.edit_preview = actions.editor->preview();
            if (!actions.editor->previewing() && !swing.settling()) result.editing = actions.editor->entry();
        }
        result.swing = &swing;
        return result;
    }
    presentation::InsertionPreview preview(const domain::Id &id) const {
        presentation::InsertionPreview result;
        if (gesture == Gesture::carrying && pressed.rope == id && !copying) {
            result.removed = source_index;
            result.removed_advance = source_advance;
        }
        if (drop_rope == id) {
            result.gap = drop;
            const auto* viewport = overview ? nullptr : &view;
            for (const auto& row : list.rows()) if (row.rope.id == id) viewport = &row.viewport;
            if (viewport) {
                const auto& card = floating_source ? *floating_source : drag.card;
                const auto g = viewport->measure(card);
                result.gap_width = g.width;
                result.gap_advance = g.advance();
                result.gap_inset = g.inset();
            }
        }
        return result;
    }
    std::vector<rendering::PaintedRow> scene() const {
        if (!overview)
            return {{&current, &view, 0, preview(current.id)}};
        std::vector<rendering::PaintedRow> result;
        if (carried_rope) {
            const auto layout = list.drag_layout(reorder_to, carried_top, carried_rope->viewport.row_height(row_height()));
            for (std::size_t i = 0; i < list.rows().size(); ++i) {
                const auto& row = list.rows()[i];
                if (row.rope.id != carried_rope->rope.id)
                    result.push_back({&row.rope, &row.viewport, layout.tops[i], preview(row.rope.id)});
            }
            // Paint the carried line and all its hangers over the displaced row.
            result.push_back({&carried_rope->rope, &carried_rope->viewport, carried_top, {}});
            return result;
        }
        float top = 0;
        for (const auto &row : list.rows()) {
            result.push_back({&row.rope, &row.viewport, top, preview(row.rope.id)});
            top += row.viewport.row_height(row_height());
        }
        return result;
    }
    void metadata() {
        const auto navigable = reads.rope_count ? reads.rope_count() : 1;
        menu_rebin = rebin_visible();
        menu_multiple = navigable > 1;
        rope_count = reads.total_rope_count ? reads.total_rope_count() : navigable;
        current = {rope, rope, 100, false};
        if (reads.rope_position && reads.ropes) {
            if (auto pos = reads.rope_position(rope)) {
                auto found = reads.ropes(*pos, 1);
                if (!found.empty())
                    current = std::move(found.front());
            }
        }
    }
    presentation::Viewport *view_of(const domain::Id &id) {
        if (!overview)
            return id == rope ? &view : nullptr;
        for (auto &row : list.rows())
            if (row.rope.id == id)
                return &row.viewport;
        return nullptr;
    }
    std::optional<rendering::PaintedRow> lane(POINT pt) const {
        const float x = dip(pt.x), y = dip(pt.y);
        if (x < 0 || x >= width || y < 0 || y >= height)
            return {};
        const auto rows = scene();
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            const auto& row = *it;
            const float local = y - row.top;
            if (presentation::RopePlaque(curve, row.top).contains(x, y) ||
                (local >= curve.y(x) - 14 && local < row.view->row_height(row_height()) - 6))
                return row;
        }
        return {};
    }
    struct Slot {
        rendering::PaintedRow row;
        std::size_t index;
        const application::CardSummary &card;
        presentation::CardFrame frame;
    };
    std::optional<Slot> slot(const domain::Id &id) const {
        for (const auto &row : scene()) {
            auto i = row.view->first_realized();
            for (const auto &card : row.view->cards()) {
                if (card.entry.id == id && near_view(row.view->left(i), row.view->geometry(i).width))
                    return Slot{row, i, card, {row.view->left(i) + row.view->geometry(i).width / 2,
                                row.top + curve.contact(row.view->left(i) + row.view->geometry(i).width / 2), float(swing.angle(id))}};
                ++i;
            }
        }
        return {};
    }
    RECT editor_bounds() const {
        RECT paper{};
        const auto source = slot(actions.editor->entry());
        if (source) {
            const auto& row = source->row;
            if (auto index = row.preview.position(source->index, row.rope->slot_limit())) {
                paper = card_rect(*row.view, source->index, row.top, row.preview, *index);
                paper.left += px(rendering::TextInset::side);
                paper.right -= px(rendering::TextInset::side);
                paper.top += px(rendering::TextInset::hanging_top);
                paper.bottom -= px(rendering::TextInset::hanging_bottom);
            }
        }
        return paper;
    }
    bool editor_visible(const presentation::Viewport& v, float top) const {
        const auto& cards = v.cards();
        const auto it = std::find_if(cards.begin(), cards.end(), [&](const auto& c) {
            return c.entry.id == actions.editor->entry();
        });
        if (it == cards.end()) return false;
        const auto r = card_rect(v, v.first_realized() + (it - cards.begin()), top);
        return r.right - px(rendering::TextInset::side) > 0 && r.left + px(rendering::TextInset::side) < px(width) &&
               r.top + px(rendering::TextInset::hanging_top) < px(height) && r.bottom > 0;
    }
    bool editor_visible(const presentation::Overview& projected) const {
        float top = 0;
        for (const auto& row : projected.rows()) {
            if (row.rope.id == editing_rope) return editor_visible(row.viewport, top);
            top += row.viewport.row_height(row_height());
        }
        return false;
    }
    bool prepare_selection(const domain::Id& id) {
        if (!editing() || id == rope) return true;
        if (overview) {
            auto next = list;
            next.open(id, width, height);
            if (editor_visible(next)) return true;
        }
        actions.editor->save([this, id] { select(id); });
        return false;
    }
    template<class Move> bool allow_scroll(presentation::Viewport& v, Move move, std::function<void()> resume) {
        if (!editing() || view_of(editing_rope) != &v) return true;
        auto next = v.preview_navigation();
        move(next);
        auto source = slot(actions.editor->entry());
        if (source && editor_visible(next, source->row.top)) return true;
        actions.editor->save(std::move(resume));
        return false;
    }
    void scroll_rows(int steps) {
        if (editing()) {
            auto next = list;
            next.scroll_rows(steps, width, height);
            if (!editor_visible(next)) {
                actions.editor->save([this, steps] { if (shown && overview) { scroll_rows(steps); update_region(); } });
                return;
            }
        }
        list.scroll_rows(steps, width, height);
    }
    void sync_editor() {
        if (!editing() || actions.editor->previewing() || !shown || swing.settling()) return;
        RECT paper = editor_bounds(), bounds{}, clip{};
        GetClientRect(hwnd, &bounds);
        if (!IntersectRect(&clip, &bounds, &paper)) return;
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&paper), 2);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&clip), 2);
        actions.editor->place(paper, clip);
    }
    // Tests a point against one card in its swung frame: the paper, its close
    // control, then the wooden bar. The hook and the gaps stay transparent.
    Part card_part(const presentation::Viewport &v, std::size_t original, std::size_t index,
                   const presentation::InsertionPreview &p, float top, const domain::Id &id, float x,
                   float y, ResizeEdge& edge) const {
        const auto g = v.geometry(original);
        const float hook_x = v.projected_left(index, p) + g.width / 2;
        const auto angle = gesture == Gesture::pending && pressed.id == id ? held_angle : float(swing.angle(id));
        const presentation::CardFrame frame{hook_x, top + curve.contact(hook_x), angle};
        float cx{}, cy{};
        frame.to_card(x, y, cx, cy);
        const float half = g.body_width / 2, bar = g.hanger_width() / 2;
        if (g.adaptive && cx >= bar - 51 && cx < bar - 25 && cy >= g.bar_bottom - 25 && cy < g.bar_bottom)
            return Part::erase;
        if (cx >= -half && cx < half && cy >= g.top && cy < g.bottom) {
            const bool close = !g.adaptive && cx >= half - Hanging::close_size && cy >= Hanging::paper_top + 4 &&
                               cy < Hanging::paper_top + Hanging::close_size;
            if (close) return Part::erase;
            if (actions.resize && !(editing() && actions.editor->previewing())) {
                edge = resize_edge(cx + half, cy - g.top, g.body_width, g.height(), false);
                if (edge) return Part::resize;
            }
            return Part::paper;
        }
        if (hovered == id && !g.adaptive && cx >= bar - 27 && cx < bar - 3 &&
            cy >= Hanging::bar_top && cy < g.bar_bottom &&
            v.cards()[original - v.first_realized()].menu == application::CardMenuKind::text)
            return Part::pencil;
        if (cx >= -bar && cx < bar && cy >= Hanging::bar_top && cy < g.bar_bottom)
            return Part::paper;
        return Part::gap;
    }
    Hit hit(POINT client) const {
        if (frames.failed()) return {};
        const float x = dip(client.x), y = dip(client.y);
        const auto rows = scene();
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            const auto& row = *it;
            const auto &v = *row.view;
            const float local = y - row.top;
            if (local < 0 || local >= row.view->row_height(row_height()))
                continue;
            auto index = v.first_realized();
            for (const auto &card : v.cards()) {
                const auto pos = row.preview.position(index++, row.rope->slot_limit());
                if (!pos || card.entry.id == feedback().dragged)
                    continue;
                ResizeEdge edge;
                if (const auto part = card_part(v, index - 1, *pos, row.preview, row.top, card.entry.id, x, y, edge); part != Part::gap)
                    return {part, card.entry.id, row.rope->id, edge};
            }
            if (presentation::RopePlaque(curve, row.top).contains(x, y) ||
                std::abs(local - curve.y(x)) <= Hanging::rope_thickness / 2 + 2.5f)
                return {Part::select, {}, row.rope->id};
        }
        return {};
    }
    // The layered HWND may not receive WM_PAINT for an invalidated region.
    void redraw() {
        ValidateRect(hwnd, nullptr);
        present();
    }
    void show_layout() {
        layout_pending = true;
        sync_hover();
        redraw();
    }
    void update_region() {
        row_input.navigation(!any_editor() && gesture == Gesture::idle && pressed.part == Part::gap && !floating_source);
        if (carried_rope || resize) {
            // Capture owns the mouse. Let the layered frame's alpha describe
            // the moving art instead of rebuilding an HWND region on every move.
            if (!installed_shape.empty()) {
                if (!SetWindowRgn(hwnd, nullptr, FALSE))
                    throw std::runtime_error("Could not release workspace hit region");
                installed_shape.clear();
                row_input.bounds({});
            }
            show_layout();
            return;
        }
        const auto rows = scene();
        RECT wheel_bounds{};
        GetClientRect(hwnd, &wheel_bounds);
        if (!rows.empty())
            wheel_bounds.bottom = std::min<LONG>(wheel_bounds.bottom, px(rows.back().top + rows.back().view->row_height(row_height())));
        else wheel_bounds.bottom = 0;
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&wheel_bounds), 2);
        row_input.bounds(wheel_bounds);
        std::vector<float> shape{float(dpi), width, height, float(overview)};
        for (const auto &row : rows) {
            const auto &v = *row.view;
            shape.insert(shape.end(), {row.top, v.paper()});
            for (std::size_t i = 0; i < v.cards().size(); ++i) {
                const auto pos = row.preview.position(v.first_realized() + i, row.rope->slot_limit());
                if (!pos) continue;
                const auto g = v.geometry(v.first_realized() + i);
                const float x = v.projected_left(*pos, row.preview);
                if (near_view(x, g.width)) shape.insert(shape.end(), {x, g.width, g.bottom});
            }
            shape.push_back(-v.paper() - 1); // Outside the range of any visible card.
        }
        if (shape == installed_shape) {
            show_layout();
            return;
        }
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (!region)
            throw std::runtime_error("Unable to allocate workspace hit region");
        try {
            // The region only bounds the art. Per-pixel alpha decides which of
            // these pixels take input, so a swinging card needs no new region.
            for (const auto &row : rows) {
                const auto &v = *row.view;
                const float top = row.top;
                const presentation::RopePlaque plaque(curve, top);
                unite(region, CreateRectRgn(px(plaque.left), px(plaque.top),
                    px(plaque.left + plaque.width), px(plaque.top + plaque.height)));
                std::vector<POINT> band;
                const int steps = std::max(2, static_cast<int>(std::ceil(width / 48)));
                for (int s = 0; s <= steps; ++s) {
                    const float x = width * s / steps;
                    band.push_back({px(x), px(top + curve.y(x) - 7)});
                }
                for (int s = steps; s >= 0; --s) {
                    const float x = width * s / steps;
                    band.push_back({px(x), px(top + curve.y(x) + 10)});
                }
                unite(region, CreatePolygonRgn(band.data(), static_cast<int>(band.size()), WINDING));
                for (std::size_t s = 0; s < v.cards().size(); ++s) {
                    const auto index = row.preview.position(v.first_realized() + s, row.rope->slot_limit());
                    if (!index) continue;
                    const auto g = v.geometry(v.first_realized() + s);
                    const float x = v.projected_left(*index, row.preview), pad = g.carried_padding();
                    if (!near_view(x, g.width)) continue;
                    const float r = top + curve.contact(x + g.width / 2);
                    const RECT art{px(x - pad), px(r + Hanging::hook_top - 4), px(x + g.width + pad),
                                   px(std::min(top + v.row_height(row_height()), r + g.bottom + g.width * .04f + 14))};
                    unite(region, CreateRectRgnIndirect(&art));
                }
            }
            if (!SetWindowRgn(hwnd, region, TRUE))
                throw std::runtime_error("Could not install workspace hit region");
            installed_shape = std::move(shape);
        } catch (...) {
            DeleteObject(region);
            throw;
        }
        show_layout();
    }
    void position_window(HMONITOR monitor = nullptr, UINT new_dpi = 0) {
        if (!monitor) monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(monitor, &mi))
            throw std::runtime_error("Workspace monitor is unavailable");
        // The rope spans the work area and is anchored at both of its edges.
        UINT dx{}, dy{};
        if (new_dpi) dpi = new_dpi;
        else if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy))) dpi = dx;
        work_area = mi.rcWork;
        view.work_area = {dip(work_area.right - work_area.left), dip(work_area.bottom - work_area.top)};
        list.work_area = {view.work_area.width, std::max(1.f, view.work_area.height - 48)};
        const auto top = !overview && actions.rope_top ? actions.rope_top(rope) : 24.;
        const auto r = rope_placement(work_area, dpi, view.row_height(row_height()), top, overview);
        sizing = true;
        const bool placed =
            SetWindowPos(hwnd, topmost ? HWND_TOPMOST : nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOACTIVATE | (topmost ? 0 : SWP_NOZORDER)) != FALSE;
        sizing = false;
        if (!placed)
            throw std::runtime_error("Unable to place workspace");
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        resized(dip(bounds.right), dip(bounds.bottom));
    }
    void mode(bool enable) {
        if (enable == overview || (enable && (!reads.rope_count || !reads.ropes || !reads.rope_position)))
            return;
        if (editing() && !enable && editing_rope != rope) {
            actions.editor->save([this, enable] { mode(enable); });
            return;
        }
        if (!enable && rope != domain::rebin_id && rebin_visible())
            actions.set_rebin_visible(false);
        if (gesture != Gesture::idle || pressed.part != Part::gap || floating_source)
            invalidate_gesture();
        overview = enable;
        wheel_remainder = 0;
        if (!shown)
            return;
        if (overview) {
            position_window();
            view.release();
            list.open(rope, width, height);
        } else {
            list.release();
            metadata();
            view.open(rope, width);
            position_window();
        }
        if (editing()) {
            if (auto* row = view_of(editing_rope)) row->pin_entry(actions.editor->entry());
        }
        update_region();
        if (actions.find_scope_changed) actions.find_scope_changed();
    }
    void active(domain::Id id) {
        if (gesture != Gesture::idle || pressed.part != Part::gap || floating_source)
            invalidate_gesture();
        rope = std::move(id);
        metadata();
        if (!shown)
            return;
        if (overview)
            list.open(rope, width, height);
        else {
            view.open(rope, width);
            position_window();
        }
        update_region();
        if (actions.find_scope_changed) actions.find_scope_changed();
    }
    void select(const domain::Id &id) {
        if (actions.select)
            actions.select(id);
        else
            active(id);
    }
    bool rebin_visible() const { return reads.rebin_visible && reads.rebin_visible(); }
    void cycle(const domain::Id& from, bool previous) {
        if (!overview && from == domain::rebin_id) {
            actions.set_rebin_visible(false);
            return;
        }
        if (!reads.rope_position || !reads.rope_count || !reads.ropes)
            return;
        auto pos = reads.rope_position(from);
        const auto count = reads.rope_count();
        if (!pos || count < 2)
            return;
        const auto next = previous ? (*pos + count - 1) % count : (*pos + 1) % count;
        auto page = reads.ropes(next, 1);
        if (!page.empty())
            select(page.front().id);
    }
    void shortcut(WorkspaceShortcut command) {
        if (!shown || any_editor() || gesture != Gesture::idle || pressed.part != Part::gap || floating_source) return;
        switch (command) {
        case WorkspaceShortcut::find: {
            POINT point{};
            GetCursorPos(&point);
            RECT area{};
            GetWindowRect(hwnd, &area);
            if (!PtInRect(&area, point)) point = {area.left + 20, area.top + 20};
            if (actions.find) actions.find(point);
            break;
        }
        case WorkspaceShortcut::next_rope: cycle(rope, false); break;
        case WorkspaceShortcut::previous_rope: cycle(rope, true); break;
        case WorkspaceShortcut::toggle_overview: mode(!overview); break;
        default: break;
        }
    }
    bool open() {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = class_name;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        // Per-pixel alpha shapes the rope, hooks, shadows and swinging cards.
        if (!CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_APPWINDOW | (topmost ? WS_EX_TOPMOST : 0) | WS_EX_LAYERED, class_name,
                             L"ClipRope", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this))
            return false;
        dpi = GetDpiForWindow(hwnd);
        set_application_icons(hwnd, dpi);
        return true;
    }
    void end_press() {
        stop_card_scroll();
        KillTimer(hwnd, rope_edge_timer);
        if (escape_registered) {
            UnregisterHotKey(hwnd, 1);
            escape_registered = false;
        }
        pressed = {};
        drop.reset();
        drop_rope.clear();
        copying = false;
        if (!drag.input() && GetCapture() == hwnd)
            ReleaseCapture();
        drag.release_input();
    }
    void reserve_drag(float width) {
        view.reserve_drag(width);
        for (auto& row : list.rows()) row.viewport.reserve_drag(width);
    }
    void cancel() {
        ++gesture_epoch;
        if (resize) {
            if (auto* row = view_of(pressed.rope)) *row = std::move(resize->original);
            resize.reset();
            if (shown && overview) list.reflow(width, height);
            else if (shown) fit_single_height();
        }
        stop_card_scroll();
        browsed_during_drag = false;
        edge_waiting_for_editor.clear();
        floating_source.reset();
        floating_capture = nullptr;
        reserve_drag(0);
        carried_rope.reset();
        gesture = Gesture::idle;
        end_press();
        drag.close();
        row_input.navigation(!any_editor() && !floating_source);
    }
    // Swing is visual feedback only. It starts frames while a card moves and
    // stops them once every card settles.
    void animate() {
        if (!shown || (!swing.moving() && gesture != Gesture::landing) || animating)
            return;
        animating = SetTimer(hwnd, swing_timer, 16, nullptr) != 0;
        last_frame = std::chrono::steady_clock::now();
    }
    double advance_swing() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - last_frame).count();
        last_frame = now;
        swing.advance(elapsed);
        return elapsed;
    }
    void step_swing() {
        const bool overlay_frame = gesture == Gesture::carrying || gesture == Gesture::landing;
        const bool redraw_rope = !overlay_frame || swing.moving_except(drag.card.entry.id);
        const auto elapsed = advance_swing();
        if (gesture == Gesture::carrying)
            drag.follow(cursor, float(swing.angle(drag.card.entry.id)));
        if (gesture == Gesture::landing)
            step_landing(elapsed);
        if (pending_previews && gesture == Gesture::idle) {
            pending_previews = false;
            if (overview) list.refresh(width, height);
            else view.reload_previews();
            update_region();
        } else if (redraw_rope || gesture == Gesture::idle) {
            sync_hover();
            redraw();
        }
        if (!swing.moving() && gesture != Gesture::landing) {
            KillTimer(hwnd, swing_timer);
            animating = false;
        }
    }
    void sway(const presentation::Viewport &v, float shift) {
        // Cards trail the rope's motion, so their papers lag behind it.
        const double strength = std::clamp(double(shift) / v.step(), -1.5, 1.5) * 22;
        if (std::abs(strength) < 0.5)
            return;
        for (const auto &card : v.cards())
            swing.kick(card.entry.id, strength);
        animate();
    }
    bool scroll(presentation::Viewport &v, double delta) {
        if (!allow_scroll(v, [&](auto& next) { next.wheel(delta, pending_previews); },
            [this, id = editing_rope, delta] {
                if (auto* row = view_of(id); shown && row) {
                    scroll(*row, delta);
                    if (overview) list.refresh(width, height);
                    update_region();
                }
            })) return false;
        const float before = v.left(0);
        v.wheel(delta, pending_previews);
        if (!overview) fit_single_height();
        pending_previews = false;
        sway(v, v.left(0) - before);
        return true;
    }
    void fit_single_height() {
        const auto wanted = std::min<LONG>(px(view.row_height(row_height())), work_area.bottom - work_area.top);
        if (!overview && gesture != Gesture::rope_drag && px(height) != wanted) position_window();
    }
    void stop_swing() {
        swing.clear();
        if (animating)
            KillTimer(hwnd, swing_timer);
        animating = false;
    }
    bool screenshot_body(const domain::Entry& entry) const {
        if (!shown || gesture != Gesture::idle || pressed.part != Part::gap || any_editor()) return false;
        POINT point{};
        if (!GetCursorPos(&point) || WindowFromPoint(point) != hwnd || !ScreenToClient(hwnd,&point)) return false;
        const auto target = hit(point);
        const auto source = slot(entry.id);
        if (target.part != Part::paper || target.id != entry.id || !source ||
            source->card.entry.snapshot_id != entry.snapshot_id || source->card.entry.revision != entry.revision) return false;
        const auto g = source->row.view->geometry(source->index);
        float x{},y{}; source->frame.to_card(dip(point.x),dip(point.y),x,y);
        // Part::paper also covers the wooden hanger; full previews are body-only.
        return x >= -g.body_width/2 && x < g.body_width/2 && y >= g.top && y < g.bottom;
    }
    bool sync_hover() {
        if (!shown || gesture != Gesture::idle || pressed.part != Part::gap)
            return false;
        POINT pt{};
        const bool over = GetCursorPos(&pt) && WindowFromPoint(pt) == hwnd && ScreenToClient(hwnd, &pt);
        auto part = over ? hit(pt) : Hit{};
        const auto source = !part.id.empty() ? slot(part.id) : std::nullopt;
        bool full = false;
        if (actions.screenshot_hover) {
            if (source && screenshot_body(source->card.entry)) {
                const auto g = source->row.view->geometry(source->index);
                const presentation::PointF corners[]{{-g.body_width/2,g.top},{g.body_width/2,g.top},
                    {-g.body_width/2,g.bottom},{g.body_width/2,g.bottom}};
                RECT bounds{LONG_MAX,LONG_MAX,LONG_MIN,LONG_MIN};
                for (auto corner : corners) {
                    const auto point = source->frame.from_card(corner);
                    bounds.left = std::min<LONG>(bounds.left,px(point.x)-1);
                    bounds.top = std::min<LONG>(bounds.top,px(point.y)-1);
                    bounds.right = std::max<LONG>(bounds.right,px(point.x)+1);
                    bounds.bottom = std::max<LONG>(bounds.bottom,px(point.y)+1);
                }
                MapWindowPoints(hwnd,nullptr,reinterpret_cast<POINT*>(&bounds),2);
                full = actions.screenshot_hover->offer(hwnd,preview_owner,source->card,bounds,
                    [this, entry=source->card.entry] { return screenshot_body(entry); });
            } else actions.screenshot_hover->leave(hwnd);
        }
        image_tip.update(hwnd, source && !full ? &source->card : nullptr);
        const bool close = part.part == Part::erase;
        if (part.id == hovered && close == hover_close)
            return false;
        hovered = std::move(part.id);
        hover_close = close;
        return true;
    }
    void invalidate_gesture() {
        if (actions.cancel_paste) actions.cancel_paste();
        const bool deferred_image = resize && pending_previews;
        const bool moving_rope = gesture == Gesture::rope_drag;
        const bool projected = resize.has_value() || moving_rope || gesture == Gesture::carrying || gesture == Gesture::landing || drop.has_value();
        first_card.clear();
        first_rope.clear();
        if (floating_capture && GetCapture() == floating_capture)
            SendMessageW(floating_capture, WM_CANCELMODE, 0, 0);
        cancel();
        if (deferred_image && shown) {
            pending_previews = false;
            if (overview) list.refresh(width, height);
            else { view.reload_previews(); fit_single_height(); }
        }
        if (moving_rope && shown) {
            if (overview) {
                list.open(rope, width, height);
                list.reveal(previous_first, width, height);
            }
            else SetWindowPos(hwnd, nullptr, start_bounds.left, start_bounds.top, 0, 0,
                              SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (shown && projected)
            update_region();
        else
            InvalidateRect(hwnd, nullptr, FALSE);
    }
    void hide() {
        if (actions.screenshot_hover) actions.screenshot_hover->leave(hwnd);
        row_input.suspend();
        wheel_remainder = 0;
        invalidate_gesture();
        stop_swing();
        image_tip.hide();
        hovered.clear();
        hover_close = false;
        ShowWindow(hwnd, SW_HIDE);
        shown = false;
        layout_pending = false;
        if (paint)
            paint->release_hanging();
        paint.reset();
        view.release();
        list.release();
        rendering::trim_frame_after_hide();
    }
    bool tiny_image(const Hit& target) const {
        if (target.part != Part::resize) return false;
        const auto source = slot(target.id);
        if (!source) return false;
        const auto g = source->row.view->geometry(source->index);
        return g.adaptive && std::min(g.body_width, g.height()) < 12;
    }
    void pointer_down(POINT pt, bool ctrl) {
        if (gesture == Gesture::rope_drag) invalidate_gesture();
        else cancel();
        down = pt;
        pressed = hit(pt);
        const bool tiny = tiny_image(pressed);
        if (tiny && ctrl) pressed.part = Part::paper;
        // A provisional preview can be recalled once its snapshot has committed.
        // Keep the original press target without starting a drag on the old snapshot.
        if (editing() && actions.editor->previewing() && pressed.id == actions.editor->entry() &&
            pressed.part == Part::paper) {
            first_card = pressed.id;
            pressed = {};
            if (actions.press) actions.press(first_card);
            return;
        }
        if (editing() && pressed.part != Part::resize && (pressed.id == actions.editor->entry() ||
            (pressed.part == Part::select && pressed.rope == editing_rope))) {
            pressed = {};
            actions.editor->focus();
            return;
        }
        row_input.navigation(!any_editor() && pressed.part == Part::gap);
        first_card = pressed.part == Part::paper ? pressed.id : std::string{};
        first_rope = pressed.part == Part::select ? pressed.rope : domain::Id{};
        if (pressed.part == Part::resize) {
            const auto source = slot(pressed.id);
            if (!source) { invalidate_gesture(); return; }
            const auto& g = source->row.view->geometry(source->index);
            const auto scale = source->card.scale;
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            if (tiny && actions.press) {
                // A very small image can rotate its edge under a stationary double-click.
                first_card = pressed.id;
                actions.press(first_card);
            }
            held_angle = source->frame.degrees;
            resize.emplace(Resize{*source->row.view, source->card.entry, scale, scale,
                {g.body_width / scale.width, g.height() / (g.adaptive ? scale.width : scale.height)},
                source->frame, screen, source->index, g.adaptive});
            gesture = Gesture::resizing;
            image_tip.hide();
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
                escape_registered = RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
            drag.capture(hwnd);
            SetCursor(LoadCursorW(nullptr, pressed.edge.cursor()));
            return;
        }
        if (pressed.part == Part::paper) {
            const auto source = slot(pressed.id);
            if (!source) {
                invalidate_gesture();
                return;
            }
            held_angle = source->frame.degrees;
            source->frame.to_card(dip(pt.x), dip(pt.y), drag.grip.x, drag.grip.y);
            gesture = Gesture::pending;
            physical_press = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (physical_press)
                escape_registered = RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
            drag.capture(hwnd);
        }
        if (pressed.part == Part::select) {
            if (overview) {
                for (const auto& row : scene())
                    if (row.rope->id == pressed.rope)
                        rope_grip_y = dip(pt.y) - row.top - curve.y(dip(pt.x));
            }
            gesture = Gesture::rope_pending;
            rope_down = pt;
            ClientToScreen(hwnd, &rope_down);
            GetWindowRect(hwnd, &start_bounds);
            physical_press = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (physical_press) escape_registered = RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
            drag.capture(hwnd);
        } else if (pressed.part == Part::erase || pressed.part == Part::pencil) {
            SetCapture(hwnd);
            if (GetCapture() != hwnd)
                invalidate_gesture();
        }
        if (pressed.part == Part::paper && actions.press)
            actions.press(pressed.id);
    }
    void lift(POINT pt) {
        if (physical_press && !escape_registered)
            throw std::runtime_error("Escape is unavailable for this drag");
        const auto source = slot(pressed.id);
        if (!source || source->row.rope->id != pressed.rope) {
            invalidate_gesture();
            return;
        }
        drag.colors = actions.colors;
        drag.card = source->card;
        source_index = drag.index = source->index;
        source_advance = source->row.view->advance(source->index);
        drag.rope = *source->row.rope;
        drag.geometry = source->row.view->geometry(source->index);
        if (actions.cancel_paste) actions.cancel_paste();
        gesture = Gesture::carrying;
        image_tip.hide();
        drag.open_visual(hwnd);
        swing.release(pressed.id, held_angle, 0);
        first_card.clear();
        cursor = pt;
        ClientToScreen(hwnd, &cursor);
        drag.follow(cursor, held_angle);
        drop = source_index;
        drop_rope = pressed.rope;
        reserve_drag(source_advance);
        update_region();
        animate();
    }
    double edge_speed(POINT pt) const {
        constexpr float band = 32;
        const float x = dip(pt.x);
        const float distance = std::min(x, width - x);
        if (distance < 0 || distance >= band) return 0;
        const double speed = 80 + 760 * (1 - distance / band);
        return x < width / 2 ? -speed : speed;
    }
    void stop_card_scroll() {
        if (edge_rope.empty()) return;
        KillTimer(hwnd, card_edge_timer);
        const auto id = std::exchange(edge_rope, {});
        if (std::exchange(edge_scrolled, false)) {
            if (const auto* v = view_of(id)) {
                try { reads.save_anchor(id, v->anchor()); }
                catch (const std::exception&) { if (actions.error) actions.error(); }
            }
        }
    }
    void track_card_edge(POINT pt, const std::optional<rendering::PaintedRow>& row) {
        const auto speed = edge_speed(pt);
        if (!row || !row->view->can_scroll(speed)) {
            stop_card_scroll();
            edge_waiting_for_editor.clear();
            return;
        }
        if (edge_waiting_for_editor == row->rope->id && editing()) return;
        edge_waiting_for_editor.clear();
        if (edge_rope == row->rope->id) return;
        stop_card_scroll();
        if (!SetTimer(hwnd, card_edge_timer, 16, nullptr))
            throw std::runtime_error("Could not start card edge scrolling");
        edge_rope = row->rope->id;
        last_edge_tick = std::chrono::steady_clock::now();
    }
    void step_card_scroll() {
        if (edge_rope.empty()) return;
        POINT pt = cursor;
        ScreenToClient(hwnd, &pt);
        const auto row = lane(pt);
        const auto speed = edge_speed(pt);
        if (!shown || !row || row->rope->id != edge_rope ||
            (!floating_source && gesture != Gesture::carrying) || !row->view->can_scroll(speed)) {
            stop_card_scroll();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - last_edge_tick).count();
        last_edge_tick = now;
        const double delta = speed * std::min(elapsed, 0.05);
        auto& v = *view_of(edge_rope);
        if (!allow_scroll(v, [delta](auto& next) { next.scroll_for_drag(delta); }, [this] {
            if (!shown || (gesture != Gesture::carrying && !floating_source)) return;
            POINT local = cursor;
            ScreenToClient(hwnd, &local);
            update_drop(local);
        })) {
            edge_waiting_for_editor = edge_rope;
            stop_card_scroll();
            return;
        }
        const auto old_height = v.row_height(row_height());
        v.scroll_for_drag(delta);
        edge_scrolled = true;
        browsed_during_drag = true;
        if (old_height != v.row_height(row_height())) {
            if (overview) list.reflow(width, height);
            else fit_single_height();
        }
        pt = cursor;
        ScreenToClient(hwnd, &pt);
        const auto previous = drop;
        const auto previous_rope = drop_rope;
        update_drop(pt, true);
        if (gesture == Gesture::carrying) drag.follow(cursor, float(swing.angle(pressed.id)));
        if (floating_capture && GetCapture() == floating_capture && (previous != drop || previous_rope != drop_rope))
            Floating::refresh_drag_preview(floating_capture, cursor);
    }
    std::optional<float> drop_origin() {
        if (!browsed_during_drag || !drop) return {};
        if (const auto* v = view_of(drop_rope)) {
            const auto gap = preview(drop_rope);
            return v->projected_left(*drop, gap) - gap.gap_inset;
        }
        return {};
    }
    void align_drop(const domain::Id& id, const domain::Id& target, size_t slot, std::optional<float> origin) {
        if (!shown || !origin || id.empty()) return;
        try {
            auto* v = view_of(target);
            if (!v || reads.position(target, id) != slot) return;
            v->align_drop(slot, *origin);
            if (overview) list.reflow(width, height);
            else fit_single_height();
            if (gesture == Gesture::idle) update_region();
        } catch (const std::exception&) { if (actions.error) actions.error(); }
    }
    std::optional<presentation::RopeSlot> update_drop(POINT pt, bool scrolled = false) {
        const auto row = lane(pt);
        if (gesture == Gesture::carrying || floating_source) track_card_edge(pt, row);
        std::optional<std::size_t> next;
        if (row)
            next = row->view->insertion(dip(pt.x), row->preview, row->rope->slot_limit());
        const auto next_rope = row ? row->rope->id : domain::Id{};
        if (drop != next || drop_rope != next_rope) {
            drop = next;
            drop_rope = next_rope;
            if (gesture == Gesture::carrying && row) {
                drag.index = *next;
                drag.rope = *row->rope;
                const auto geometry = row->view->measure(drag.card);
                drag.grip.x *= geometry.body_width / drag.geometry.body_width;
                drag.grip.y = geometry.top + (drag.grip.y - drag.geometry.top) * geometry.height() / drag.geometry.height();
                drag.geometry = geometry;
                drag.refresh_face();
            }
            scrolled = true;
        }
        if (scrolled) update_region();
        if (!row) return {};
        const auto& carried = floating_source ? *floating_source : drag.card;
        return presentation::RopeSlot{*row->rope, *next, row->view->measure(carried), drop_origin()};
    }
    void move_rope(POINT pt, bool edge_tick = false) {
        auto screen = pt;
        ClientToScreen(hwnd, &screen);
        cursor = screen;
        const bool starting = gesture == Gesture::rope_pending;
        if (starting) {
            if (screen.y == rope_down.y) return;
            if (physical_press && !escape_registered) throw std::runtime_error("Escape unavailable for rope drag");
            if (overview) {
                const auto pos = list.position(pressed.rope);
                if (!pos || *pos < list.first() || *pos - list.first() >= list.rows().size()) {
                    invalidate_gesture();
                    return;
                }
                const auto& source = list.rows()[*pos - list.first()];
                if (source.rope.id != pressed.rope) { invalidate_gesture(); return; }
                carried_rope = source;
                reorder_from = reorder_to = *pos;
                reorder_target = pressed.rope;
                previous_first = list.rows().front().rope.id;
                if (!SetTimer(hwnd, rope_edge_timer, 350, nullptr)) throw std::runtime_error("Could not start edge scroll");
            }
            gesture = Gesture::rope_drag;
        }
        // Small movement within the same overview row still permits a native
        // double-click. Reordering consumes the gesture.
        if (!overview || reached_drag_distance(rope_down, screen, dpi)) first_rope.clear();
        if (!overview) {
            const auto top = std::clamp(start_bounds.top + screen.y - rope_down.y, work_area.top,
                                        work_area.bottom - (start_bounds.bottom - start_bounds.top));
            if (!SetWindowPos(hwnd, nullptr, start_bounds.left, top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER))
                throw std::runtime_error("Could not move rope");
            return;
        }
        carried_top = dip(pt.y) - curve.y(dip(pt.x)) - rope_grip_y;
        if (pt.x < 0 || pt.x >= px(width) || pt.y < 0 || pt.y >= px(height)) {
            if (!edge_tick) update_region();
            return;
        }
        if (edge_tick) {
            const int step = pt.y < px(28) ? -1 : pt.y >= px(height - 28) ? 1 : 0;
            if (!step) return;
            const auto first = list.first();
            list.scroll_rows(step, width, height);
            if (list.first() == first) return;
        }
        const auto index = list.drag_layout(reorder_to, carried_top, carried_rope->viewport.row_height(row_height())).insertion;
        if (index != reorder_to) {
            first_rope.clear();
            reorder_target = list.at(index).id;
            reorder_to = index;
            list.preview_move(reorder_from, reorder_to, width, height);
        }
        update_region();
    }
    void present_rope_order(domain::Id collector) {
        if (!overview || !shown) { active(std::move(collector)); return; }
        const bool changed_collector = collector != rope;
        rope = std::move(collector);
        metadata();
        if (changed_collector) list.open(rope, width, height);
        else list.refresh(width, height); // Preserve the visible overview page when only lower rows moved.
        if (actions.find_scope_changed) actions.find_scope_changed();
        update_region();
    }
    void release_rope(POINT pt) {
        move_rope(pt);
        if (!first_rope.empty()) {
            const auto release = hit(pt);
            if (release.part != Part::select || release.rope != pressed.rope) first_rope.clear();
        }
        if (gesture == Gesture::rope_drag) {
            auto collector = rope;
            if (overview) {
                if (pt.x < 0 || pt.x >= px(width) || pt.y < 0 || pt.y >= px(height)) {
                    invalidate_gesture();
                    return;
                }
                if (reorder_target != pressed.rope && actions.reorder_rope) {
                    const auto from = pressed.rope, to = reorder_target, head = list.head();
                    const auto epoch = gesture_epoch;
                    actions.reorder_rope(from, to, head, [this, epoch](auto selected) {
                        if (gesture_epoch == epoch) {
                            list.clear_move();
                            cancel();
                        } else if (gesture != Gesture::idle) invalidate_gesture();
                        present_rope_order(std::move(selected));
                    });
                    // A deferred command left the viewport's materialized rows in
                    // insertion-preview order. Restore the committed page until ack.
                    if (gesture_epoch == epoch) invalidate_gesture();
                    return;
                }
                list.clear_move();
            } else if (actions.save_rope_top) {
                RECT bounds{};
                GetWindowRect(hwnd, &bounds);
                if (bounds.top != start_bounds.top)
                    actions.save_rope_top(pressed.rope, dip(bounds.top - work_area.top));
            }
            cancel();
            if (overview) present_rope_order(std::move(collector));
            else update_region();
        } else if (gesture == Gesture::rope_pending) {
            cancel();
        }
    }
    void pointer_move(POINT pt, bool ctrl, bool released = false) {
        if (resize) {
            ClientToScreen(hwnd, &pt);
            if (resize->current == resize->start && !reached_drag_distance(resize->screen, pt, dpi)) return;
            if (!first_card.empty()) {
                first_card.clear();
                if (actions.cancel_paste) actions.cancel_paste();
            }
            float dx{}, dy{};
            // Invert rotation for a displacement (the translation cancels).
            const presentation::CardFrame rotation{0, -Hanging::pivot, held_angle};
            rotation.to_card(dip(pt.x - resize->screen.x), dip(pt.y - resize->screen.y), dx, dy);
            const auto scale = dragged_scale(resize->start, pressed.edge, resize->base,
                                             2 * dx, dy - Hanging::pivot, resize->image);
            if (scale == resize->current) return;
            auto* row = view_of(pressed.rope);
            row->resize_card(resize->index, scale, resize->frame.hook_x);
            resize->current = scale;
            if (overview) list.reflow(width, height);
            else fit_single_height();
            update_region();
            return;
        }
        if (pressed.part == Part::select) {
            move_rope(pt);
            return;
        }
        if (gesture == Gesture::pending && reached_drag_distance(down, pt, dpi)) {
            const auto source_slot = slot(pressed.id);
            if (!source_slot || (ctrl && drag.grip.y < source_slot->row.view->geometry(source_slot->index).top)) {
                invalidate_gesture();
                return;
            }
            if (ctrl && !overview) {
                const auto source = slot(pressed.id);
                auto card = source ? std::optional(source->card) : std::nullopt;
                invalidate_gesture();
                if (!released && card && actions.external_drag)
                    actions.external_drag(std::move(*card));
                return;
            }
            copying = ctrl;
            lift(down);
        }
        if (gesture != Gesture::carrying)
            return;
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (screen.x != cursor.x) {
            swing.kick(pressed.id, std::clamp(dip(screen.x - cursor.x) * 0.35, -30., 30.));
            animate();
        }
        cursor = screen;
        update_drop(pt);
        drag.follow(cursor, float(swing.angle(pressed.id)));
    }
    void step_landing(double elapsed) {
        const auto destination = slot(drag.card.entry.id);
        if (!destination) {
            cancel();
            return;
        }
        POINT origin{};
        ClientToScreen(hwnd, &origin);
        const float x = origin.x + destination->frame.hook_x * dpi / 96;
        const float y = origin.y + destination->frame.rope * dpi / 96;
        landing_time += elapsed;
        // Critically damped return from rest, evaluated against the current slot.
        const double t = landing_time * 24;
        const float remaining = float((1 + t) * std::exp(-t));
        const float dx = (landing_start.x - x) * remaining, dy = (landing_start.y - y) * remaining;
        if (std::max(std::abs(dx), std::abs(dy)) < 0.5f) {
            cancel();
            return;
        }
        drag.place({x + dx, y + dy}, dpi, destination->frame.degrees);
    }
    void pointer_up(POINT pt, bool ctrl) {
        if (resize) {
            pointer_move(pt, false);
            if (resize->current != resize->start) actions.resize(resize->entry, resize->current, nullptr);
            resize.reset();
            cancel();
            if (overview) list.refresh(width, height);
            else if (pending_previews) { view.refresh(width); fit_single_height(); }
            pending_previews = false;
            update_region();
            return;
        }
        if (pressed.part == Part::select) {
            release_rope(pt);
            return;
        }
        if (pressed.part == Part::gap)
            return;
        pointer_move(pt, ctrl, true); // Rapid motion may coalesce all intermediate mouse moves.
        const bool copy = copying;
        const auto action = pressed;
        const auto target = drop;
        const auto target_rope = drop_rope;
        const auto origin = drop_origin();
        const bool moved = gesture == Gesture::carrying;
        const auto released = hit(pt);
        const float angle = float(swing.angle(action.id));
        end_press();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (moved) {
            // A dropped card keeps swinging the way it was carried, then settles.
            const auto toss = pt.x >= down.x ? 20. : -20.;
            auto landed_at = [this, epoch = gesture_epoch, target, target_rope, origin, angle, toss](domain::Id landed) {
                if (epoch != gesture_epoch) return;
                if (target) align_drop(landed, target_rope, *target, origin);
                const auto dest = slot(landed);
                if (gesture != Gesture::carrying || !dest || dest->row.rope->id != target_rope) {
                    cancel();
                    update_region();
                    return;
                }
                drag.card = dest->card;
                drag.rope = *dest->row.rope;
                drag.index = dest->index;
                drag.geometry = dest->row.view->geometry(dest->index);
                drag.refresh_face();
                gesture = Gesture::landing;
                landing_start = drag.hook_pixels();
                landing_time = 0;
                swing.release(landed, angle, toss);
                update_region();
                animate();
            };
            if (target && (copy || target_rope != action.rope) && actions.transfer) {
                actions.transfer(action.id, target_rope, copy, *target, std::move(landed_at));
                return;
            } else if (target && target_rope == action.rope && actions.reorder) {
                actions.reorder(action.id, *target, std::move(landed_at));
                return;
            } else if (!target && !copy && actions.detach) {
                const auto& g = drag.geometry;
                const float u = std::clamp(drag.grip.x / g.body_width + 0.5f, 0.f, 1.f);
                const float v = std::clamp((drag.grip.y - g.top) / g.height(), 0.f, 1.f);
                actions.detach(action.id, cursor, u, v);
            }
        }
        cancel();
        if (moved)
            update_region();
        if (!moved && released.part == action.part && released.id == action.id && released.rope == action.rope) {
            switch (action.part) {
            case Part::pencil:
                if (actions.edit) actions.edit(action.id);
                break;
            case Part::erase:
                if (actions.erase)
                    actions.erase(action.id, action.rope);
                break;
            default:
                break;
            }
        }
    }
    void command(WorkspaceCommand choice, const domain::Id &id) {
        auto *v = view_of(id);
        if (!shown || !v) return;
        switch (choice) {
        case WorkspaceCommand::previous: cycle(id, true); break;
        case WorkspaceCommand::next: cycle(id, false); break;
        case WorkspaceCommand::rebin:
            if (actions.set_rebin_visible) actions.set_rebin_visible(!rebin_visible());
            break;
        case WorkspaceCommand::head:
            if (v->total() && actions.head) actions.head(id);
            break;
        case WorkspaceCommand::latest:
            if (!allow_scroll(*v, [](auto& next) { next.latest(); }, [this, choice, id] { command(choice, id); })) break;
            v->latest();
            update_region();
            break;
        case WorkspaceCommand::overview:
            if (overview && rope != id) select(id);
            mode(!overview);
            break;
        case WorkspaceCommand::keyboard:
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            break;
        case WorkspaceCommand::density:
            if (editing() && editing_rope == id) {
                actions.editor->save([this, choice, id] { command(choice, id); });
                break;
            }
            v->set_compact(!v->compact());
            update_region();
            break;
        default: break;
        }
    }
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp) {
        if (actions.screenshot_hover && (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK || msg==WM_RBUTTONDOWN ||
            msg==WM_MBUTTONDOWN || msg==WM_CONTEXTMENU || msg==WM_ENTERMENULOOP || msg==WM_MOUSEWHEEL ||
            msg==WM_MOUSEHWHEEL || msg==RowInput::wheel_message || msg==RowInput::shortcut_message ||
            msg==WM_DPICHANGED || msg==WM_SIZE || msg==WM_MOVE || msg==WM_SHOWWINDOW || msg==WM_NCDESTROY))
            actions.screenshot_hover->leave(hwnd);
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (msg) {
        case WM_MOVE:
            if (shown && !sizing) update_region();
            if (shown) sync_editor();
            break;
        case WM_COMMAND:
            if (!lp) command(static_cast<WorkspaceCommand>(LOWORD(wp)), rope);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !PopupMenuHost::activates(HWND(lp)) && actions.lost_focus) actions.lost_focus(hwnd);
            break;
        case WM_SETCURSOR: {
            POINT cursor_point{};
            GetCursorPos(&cursor_point);
            ScreenToClient(hwnd, &cursor_point);
            const auto edge = resize ? pressed.edge : hit(cursor_point).edge;
            if (edge) { SetCursor(LoadCursorW(nullptr, edge.cursor())); return TRUE; }
            break;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CONTEXTMENU: {
            menu_mark(MenuStage::input);
            POINT local = point;
            if (ScreenToClient(hwnd, &local)) {
                auto part = hit(local);
                if (editing() && part.id == actions.editor->entry()) return 0;
                if ((part.part == Part::paper || part.part == Part::pencil || part.part == Part::resize) && actions.context) {
                    if (auto target = slot(part.id))
                        actions.context(application::CardMenuSnapshot(target->card,
                            std::pair{target->index + 1, target->row.view->total()}), point);
                } else if (part.part == Part::select && actions.rope_menu) {
                    for (auto& row : scene()) {
                        if (row.rope->id != part.rope) continue;
                        auto choice = actions.rope_menu(*row.rope, point, {overview, row.view->compact(),
                            row.view->total() != 0, menu_rebin, menu_multiple});
                        if (choice == WorkspaceCommand::find) {
                            if (!any_editor() && actions.find) actions.find(point);
                        } else command(choice, part.rope);
                        break;
                    }
                }
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PRINTCLIENT: {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            if (shown) {
                if (!paint)
                    paint = rendering::WorkspacePaint::acquire();
                // A printed frame shows the resting scene without pointer feedback.
                const auto rows = scene();
                rendering::Feedback still;
                still.colors = actions.colors;
                still.dragged = feedback().dragged;
                still.edit_preview = feedback().edit_preview;
                still.rope_count = rope_count;
                rendering::print_window(reinterpret_cast<HDC>(wp), bounds, [&](HDC dest) {
                    return paint->draw(dest, bounds, dpi, rows, still);
                });
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            try {
                if (layout_pending || !IsRectEmpty(&ps.rcPaint))
                    present();
            } catch (...) {
                EndPaint(hwnd, &ps);
                throw;
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DPICHANGED: {
            set_application_icons(hwnd, HIWORD(wp));
            if (sizing) return 0;
            invalidate_gesture();
            dpi = HIWORD(wp);
            const auto &r = *reinterpret_cast<const RECT *>(lp);
            if (shown) {
                position_window(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), dpi);
                if (overview) list.refresh(width, height);
                else view.refresh(width);
                update_region();
            } else SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_SIZE:
            resized(dip(LOWORD(lp)), dip(HIWORD(lp)));
            if (shown && !sizing && wp != SIZE_MINIMIZED) {
                if (overview)
                    list.refresh(width, height);
                else
                    view.refresh(width);
                update_region();
            }
            return 0;
        case WM_MOUSEWHEEL:
        case RowInput::wheel_message:
        case WM_MOUSEHWHEEL:
            if (shown && gesture == Gesture::idle) {
                auto local = point;
                ScreenToClient(hwnd, &local);
                if (editing() && hit(local).id == actions.editor->entry()) {
                    actions.editor->wheel(wp);
                    return 0;
                }
                const auto rows = scene();
                if (local.x < 0 || local.x >= px(width) || local.y < 0 || local.y >= px(height) || rows.empty() ||
                    dip(local.y) >= rows.back().top + rows.back().view->row_height(row_height())) return 0;
                // The scroll presents its own frame. Sample motion for that frame
                // and resume timer-driven drawing after the last wheel event.
                if (animating && gesture == Gesture::idle)
                    advance_swing();
                const float dir = msg == WM_MOUSEHWHEEL ? 1.f : -1.f;
                const auto delta = GET_WHEEL_DELTA_WPARAM(wp);
                if (msg != WM_MOUSEHWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)) {
                    wheel_remainder += delta;
                    const int steps = wheel_remainder / WHEEL_DELTA;
                    wheel_remainder %= WHEEL_DELTA;
                    if (!steps) return 0;
                    if (overview) scroll_rows(-steps);
                    else if (rope == domain::rebin_id) cycle(rope, steps > 0);
                    else for (int i = 0; i < std::abs(steps); ++i) cycle(rope, steps > 0);
                } else if (overview) {
                    float top = 0;
                    for (auto &row : list.rows()) {
                        const float bottom = top + row.viewport.row_height(row_height());
                        if (dip(local.y) >= top && dip(local.y) < bottom) {
                            if (!scroll(row.viewport, dir * delta / WHEEL_DELTA)) return 0;
                            list.refresh(width, height);
                            break;
                        }
                        top = bottom;
                    }
                } else if (!scroll(view, dir * delta / WHEEL_DELTA)) return 0;
                update_region();
                if (animating && gesture == Gesture::idle)
                    SetTimer(hwnd, swing_timer, 16, nullptr);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const auto part = msg == WM_LBUTTONDBLCLK ? hit(point) : Hit{};
            if (begins_card_press(msg, part.part == Part::erase)) {
                pointer_down(point, (wp & MK_CONTROL) != 0);
            } else if (!(wp & MK_CONTROL) && part.id == first_card && tiny_image(part) && actions.double_click) {
                first_card.clear();
                actions.double_click(part.id);
            } else if (part.part == Part::resize || (part.part == Part::paper && (wp & MK_CONTROL))) {
                pointer_down(point, (wp & MK_CONTROL) != 0);
            } else if (part.part == Part::paper && part.id == first_card && actions.double_click) {
                first_card.clear();
                actions.double_click(part.id);
            } else if (overview && part.part == Part::select && part.rope == first_rope) {
                first_rope.clear();
                select(part.rope);
                if (rope == part.rope) mode(false);
            } else if (part.part == Part::paper || part.part == Part::select)
                pointer_down(point, (wp & MK_CONTROL) != 0);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (wp & MK_LBUTTON)
                pointer_move(point, (wp & MK_CONTROL) != 0);
            else if (shown && gesture == Gesture::idle) {
                // Re-entry can arrive before the previous leave is dispatched.
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                if (sync_hover())
                    redraw();
            }
            return 0;
        case WM_MOUSELEAVE:
            // A leave from a transparent gap may arrive after re-entry.
            if (sync_hover())
                redraw();
            return 0;
        case WM_LBUTTONUP:
            pointer_up(point, (wp & MK_CONTROL) != 0);
            return 0;
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
            if (GetFocus() == hwnd) {
                const auto command = match_scoped(scoped_keys, {current_modifiers(), UINT(wp)});
                if (command != WorkspaceShortcut::none) {
                    if (!(lp & (LPARAM(1) << 30))) shortcut(command);
                    return 0;
                }
            }
            if (wp == VK_ESCAPE) {
                invalidate_gesture();
                return 0;
            }
            break;
        case RowInput::shortcut_message:
            if (row_input.accepts(lp)) shortcut(static_cast<WorkspaceShortcut>(wp));
            return 0;
        case WM_TIMER:
            if (wp == card_edge_timer) {
                step_card_scroll();
                return 0;
            }
            if (wp == rope_edge_timer && overview && gesture == Gesture::rope_drag) {
                POINT pt = cursor;
                ScreenToClient(hwnd, &pt);
                move_rope(pt, true);
                return 0;
            }
            if (wp == swing_timer) {
                step_swing();
                return 0;
            }
            return 0;
        case WM_CANCELMODE:
            invalidate_gesture();
            return 0;
        case WM_HOTKEY:
            if (wp == 1 && escape_registered) {
                invalidate_gesture();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (pressed.part != Part::gap && (!drag.input() || reinterpret_cast<HWND>(lp) != drag.input()))
                invalidate_gesture();
            return 0;
        case WM_CLOSE:
            if (actions.hide)
                actions.hide();
            return 0;
        case WM_NCDESTROY: {
            const auto old = hwnd;
            hwnd = nullptr;
            SetWindowLongPtrW(old, GWLP_USERDATA, 0);
            return DefWindowProcW(old, msg, wp, lp);
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto *impl = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            impl = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            impl->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
        }
        if (!impl)
            return DefWindowProcW(hwnd, msg, wp, lp);
        try {
            return impl->message(msg, wp, lp);
        } catch (const std::exception &) {
            if (impl->gesture != Gesture::idle || impl->floating_source)
                impl->invalidate_gesture();
            if (impl->actions.error)
                impl->actions.error();
            return 0;
        }
    }
};

Workspace::Workspace(WorkspaceReads reads, WorkspaceActions actions)
    : impl_(std::make_unique<Impl>(std::move(reads), std::move(actions))) {}
Workspace::~Workspace() = default;
bool Workspace::open() { return impl_->hwnd || impl_->open(); }
application::FindScope Workspace::find_scope() const {
    return {impl_->overview ? impl_->list.head() : impl_->rope, impl_->overview, impl_->menu_rebin};
}
void Workspace::always_on_top(bool enabled) {
    if (impl_->topmost == enabled) return;
    if (impl_->hwnd && !SetWindowPos(impl_->hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
        throw std::runtime_error("Windows could not change the workspace stacking order.");
    impl_->topmost = enabled;
}
void Workspace::match_color(unsigned rgb) {
    impl_->match_color = rgb;
    if (visible() && !impl_->find_term.empty()) impl_->redraw();
}
void Workspace::find_highlights(std::wstring term, std::optional<application::SearchHit> hit) {
    application::fold_find(term);
    impl_->find_term = std::move(term);
    impl_->find_hit = std::move(hit);
    if (visible()) impl_->redraw();
}
bool Workspace::reveal_match(const application::SearchHit& hit) {
    if (!visible() || impl_->editing() || impl_->gesture != Gesture::idle || impl_->floating_source) return true;
    auto position = impl_->reads.position(hit.rope, hit.entry);
    if (!position || (!impl_->overview && impl_->rope != hit.rope)) return false;
    if (impl_->overview)
        impl_->list.reveal_card(hit.rope, *position, impl_->width, impl_->height);
    else {
        impl_->view.reveal(*position);
        impl_->fit_single_height();
    }
    auto card = impl_->slot(hit.entry);
    if (!card || card->card.entry.snapshot_id != hit.snapshot || card->row.rope->id != hit.rope) return false;
    impl_->update_region();
    return true;
}
void Workspace::shortcuts(const ScopedKeys& keys) {
    impl_->scoped_keys = keys;
    impl_->row_input.configure(keys);
}
std::optional<DWORD> Workspace::hover_keyboard_status() const {
    return impl_->row_input.keyboard_status();
}
void Workspace::show(std::string rope_id) {
    if (!impl_->hwnd)
        throw std::logic_error("Workspace has not been opened");
    impl_->rope = std::move(rope_id);
    impl_->position_window(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY));
    impl_->metadata();
    if (impl_->overview)
        impl_->list.open(impl_->rope, impl_->width, impl_->height);
    else {
        impl_->view.open(impl_->rope, impl_->width);
        impl_->position_window();
    }
    const auto shortcut_error = impl_->row_input.start(impl_->hwnd);
    impl_->shown = true;
    for (const auto &row : impl_->scene()) {
        int direction = 1;
        for (const auto &card : row.view->cards()) {
            impl_->swing.kick(card.entry.id, direction * 24);
            direction = -direction;
        }
    }
    impl_->animate();
    // A layered window reappears with its last presented frame, so this presents the new scene first.
    impl_->update_region();
    if (!impl_->topmost)
        SetWindowPos(impl_->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(impl_->hwnd, SW_SHOWNOACTIVATE);
    impl_->sync_editor();
    UpdateWindow(impl_->hwnd);
    if (shortcut_error && impl_->actions.shortcut_error) impl_->actions.shortcut_error(shortcut_error);
}
void Workspace::hide() { impl_->hide(); }
void Workspace::preview_changed(const application::PreviewChanges& changes) {
    if (!impl_->shown) return;
    if (!changes.all) {
        const auto rows = impl_->scene();
        const bool realized = std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
            const auto& cards = row.view->cards();
            return std::any_of(cards.begin(), cards.end(), [&](const auto& card) { return changes.contains(card.preview_key()); });
        });
        if (!realized && !changes.contains(impl_->drag.card.preview_key())) return;
    }
    // Incorporate worker completions in the next wheel or spring frame.
    // Presenting a separate full layered window for each header and bitmap
    // would frequently draw the same scroll position three times.
    if (impl_->resize || (impl_->animating && impl_->gesture == Gesture::idle)) impl_->pending_previews = true;
    else refresh();
}
void Workspace::repaint_colors() {
    impl_->drag.repaint_colors();
    if (impl_->shown) impl_->redraw();
}
void Workspace::refresh() {
    if (impl_->resize) impl_->invalidate_gesture();
    impl_->pending_previews = false;
    if (impl_->editing() && !impl_->reads.position(impl_->editing_rope, impl_->actions.editor->entry()))
        impl_->actions.editor->discard();
    if (!impl_->shown)
        return;
    if (impl_->overview && impl_->gesture == Gesture::rope_drag &&
        (impl_->list.position(impl_->pressed.rope) != impl_->reorder_from ||
         impl_->list.position(impl_->reorder_target) != impl_->reorder_to))
        impl_->invalidate_gesture();
    impl_->metadata();
    const auto head = [](const presentation::Viewport &v) {
        return v.first_realized() == 0 && !v.cards().empty() ? v.cards().front().entry.id : domain::Id{};
    };
    std::unordered_map<domain::Id, domain::Id> previous_heads;
    for (const auto &row : impl_->scene())
        previous_heads.emplace(row.rope->id, head(*row.view));
    if (impl_->overview) {
        impl_->list.refresh(impl_->width, impl_->height);
        if (impl_->carried_rope) {
            auto& carried = *impl_->carried_rope;
            const auto& rows = impl_->list.rows();
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.rope.id == carried.rope.id; });
            if (found != rows.end()) carried = *found;
            else carried.viewport.refresh(impl_->width);
        }
    } else {
        impl_->view.refresh(impl_->width);
        impl_->fit_single_height();
    }
    if ((impl_->gesture == Gesture::carrying || impl_->floating_source) &&
        !impl_->drop_rope.empty() && !impl_->view_of(impl_->drop_rope))
        impl_->invalidate_gesture();
    if (impl_->gesture == Gesture::carrying && !impl_->pressed.id.empty()) {
        const auto position = impl_->reads.position(impl_->pressed.rope, impl_->pressed.id);
        if (!position)
            impl_->invalidate_gesture();
        else {
            impl_->source_index = *position;
            POINT local = impl_->cursor;
            ScreenToClient(impl_->hwnd, &local);
            impl_->update_drop(local);
        }
    }
    for (const auto &row : impl_->scene()) {
        const auto before = previous_heads.find(row.rope->id);
        if (const auto after = head(*row.view); before != previous_heads.end() && !after.empty() && after != before->second) {
            impl_->swing.kick(after, 26);
            impl_->animate();
        }
    }
    impl_->update_region();
}
void Workspace::retranslate() {
    const auto& labels = ui_localization();
    impl_->view.retranslate(labels);
    for (auto& row : impl_->list.rows()) row.viewport.retranslate(labels);
    if (impl_->carried_rope) impl_->carried_rope->viewport.retranslate(labels);
    impl_->drag.retranslate(labels);
    impl_->image_tip.hide();
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
void Workspace::edit_text(application::TextDraft draft, InlineEditorHost::Save save) {
    if (!visible() || !impl_->actions.editor) return;
    const auto source = impl_->slot(draft.entry_id);
    if (!source || source->card.entry.revision != draft.revision) return;
    impl_->editing_rope = source->row.rope->id;
    impl_->actions.editor->bind(impl_->hwnd, std::move(draft), source->card, std::move(save),
        [this] { return impl_->present(); }, [this](const application::CardSummary* saved) {
        impl_->swing.unpin();
        if (auto* view = impl_->view_of(impl_->editing_rope)) {
            if (saved) view->accept_text_edit(*saved);
            view->pin_entry({});
        }
        impl_->editing_rope.clear();
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
    });
    const auto& id = impl_->actions.editor->entry();
    impl_->swing.pin(id);
    impl_->view_of(impl_->editing_rope)->pin_entry(id);
    impl_->animate();
    impl_->redraw();
}
void Workspace::editor_changed() {
    impl_->row_input.navigation(!impl_->any_editor() && impl_->gesture == Gesture::idle &&
                                impl_->pressed.part == Part::gap && !impl_->floating_source);
}
bool Workspace::visible() const {
    return impl_->shown && (GetWindowLongPtrW(impl_->hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
}
void Workspace::display_changed() {
    impl_->invalidate_gesture();
    if(!impl_->shown)return;
    impl_->position_window();
    refresh();
}
void Workspace::image_settings(domain::ImagePreferences prefs) {
    impl_->image_preferences = prefs;
    impl_->list.row_height = presentation::image_row_height(prefs.size);
    display_changed();
}
unsigned Workspace::picture_width(const domain::Id&) const {
    return std::max(1, impl_->px(presentation::image_limit(impl_->image_preferences.size).width));
}
HWND Workspace::window() const { return impl_->hwnd; }
std::shared_ptr<CardPressInput> Workspace::track_card_press() { return impl_->row_input.track_card_press(impl_->drag.input()); }
std::uint64_t Workspace::painted_frames() const {return impl_->paints;}
std::size_t Workspace::realized_count() const {
    std::size_t count = 0;
    for (const auto &row : impl_->scene())
        count += row.view->cards().size();
    return count;
}
bool Workspace::prepare_selection(const domain::Id& rope) { return impl_->prepare_selection(rope); }
void Workspace::set_active(domain::Id rope) { impl_->active(std::move(rope)); }
void Workspace::set_overview(bool enabled) { impl_->mode(enabled); }
bool Workspace::in_overview() const { return impl_->overview; }
RECT Workspace::card_screen_rect(std::size_t index) const {
    RECT card = impl_->card_rect(impl_->view, index, 0), bounds{}, clipped{};
    GetClientRect(impl_->hwnd, &bounds);
    if (index >= impl_->view.total() || !IntersectRect(&clipped, &card, &bounds))
        return {};
    MapWindowPoints(impl_->hwnd, nullptr, reinterpret_cast<POINT *>(&clipped), 2);
    return clipped;
}
std::vector<std::pair<domain::Id, RECT>> Workspace::slots() const {
    std::vector<std::pair<domain::Id, RECT>> slots;
    RECT bounds{};
    GetClientRect(impl_->hwnd, &bounds);
    for (const auto &row : impl_->scene()) {
        auto pos = row.view->first_realized();
        for (const auto &card : row.view->cards()) {
            const auto index = row.preview.position(pos++, row.rope->slot_limit());
            if (!index || (impl_->gesture == Gesture::carrying && !impl_->copying && impl_->drag.card.entry.id == card.entry.id))
                continue;
            const auto r = impl_->card_rect(*row.view, pos - 1, row.top, row.preview, *index);
            RECT clipped{};
            if (!IntersectRect(&clipped, &r, &bounds))
                continue;
            MapWindowPoints(impl_->hwnd, nullptr, reinterpret_cast<POINT *>(&clipped), 2);
            slots.emplace_back(card.entry.id, clipped);
        }
    }
    return slots;
}
std::vector<std::pair<domain::Id, RECT>> Workspace::rows() const {
    std::vector<std::pair<domain::Id, RECT>> result;
    RECT bounds{};
    GetClientRect(impl_->hwnd, &bounds);
    // Each band follows the rope at its lowest point, where the sag settles.
    const float low = impl_->curve.lowest(), half = Hanging::rope_thickness / 2;
    for (const auto &row : impl_->scene()) {
        RECT r{0, impl_->px(row.top + low - half), impl_->px(impl_->width), impl_->px(row.top + low + half)},
            clipped{};
        if (!IntersectRect(&clipped, &r, &bounds))
            continue;
        MapWindowPoints(impl_->hwnd, nullptr, reinterpret_cast<POINT *>(&clipped), 2);
        result.emplace_back(row.rope->id, clipped);
    }
    if (impl_->carried_rope)
        std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.second.top < b.second.top; });
    return result;
}
std::optional<domain::Id> Workspace::rope_at(POINT point) const {
    if (!visible() || !ScreenToClient(impl_->hwnd, &point))
        return {};
    auto row = impl_->lane(point);
    return row ? std::optional(row->rope->id) : std::nullopt;
}
std::optional<presentation::RopeSlot> Workspace::preview_floating(POINT point, const application::CardSummary* card) {
    const auto screen = point;
    if (!visible() || !ScreenToClient(impl_->hwnd, &point))
        return {};
    if (impl_->gesture == Gesture::landing)
        impl_->invalidate_gesture();
    if (impl_->gesture != Gesture::idle)
        return {};
    if (card && impl_->lane(point) && (!impl_->floating_source || impl_->floating_source->entry.id != card->entry.id)) {
        impl_->floating_source = *card;
        impl_->floating_capture = GetCapture();
        impl_->row_input.navigation(false);
        impl_->reserve_drag(presentation::ImageCardLayout::measure(*card).advance());
    }
    impl_->cursor = screen;
    return impl_->update_drop(point);
}
void Workspace::clear_floating_preview(bool repaint) {
    if (impl_->floating_source) {
        impl_->stop_card_scroll();
        impl_->edge_waiting_for_editor.clear();
        impl_->browsed_during_drag = false;
        impl_->reserve_drag(0);
    }
    impl_->floating_source.reset();
    impl_->floating_capture = nullptr;
    impl_->row_input.navigation(!impl_->any_editor() && impl_->gesture == Gesture::idle && impl_->pressed.part == Part::gap);
    if (impl_->gesture == Gesture::idle && impl_->drop) {
        impl_->drop.reset();
        impl_->drop_rope.clear();
        if (repaint && visible()) impl_->update_region();
    }
}
void Workspace::settle_floating_drop(const domain::Id& landed, const presentation::RopeSlot& slot) {
    impl_->align_drop(landed, slot.rope.id, slot.index, slot.scrolled_origin);
}
} // namespace cliprope::win
