#include "platform_win/floating.hpp"
#include "platform_win/workspace.hpp"
#include "presentation/color_preview.hpp"
#ifdef CLIPROPE_DRAW_PROBES
#include "rendering/workspace_paint.hpp"
#include "workspace_fixture_frame.hpp"
#include "management_frame.hpp"
#include "platform_win/menu_bitmap.hpp"
#include <chrono>
#endif
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace cliprope;
namespace {
#ifdef CLIPROPE_DRAW_PROBES
std::filesystem::path color_output;
#endif
void require(bool ok, const char *reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
void send(HWND hwnd, UINT msg, POINT p, WPARAM w = 0) { SendMessageW(hwnd, msg, w, MAKELPARAM(p.x, p.y)); }
void click(HWND hwnd, POINT p) {
    send(hwnd, WM_LBUTTONDOWN, p, MK_LBUTTON);
    send(hwnd, WM_LBUTTONUP, p);
}
POINT at(HWND hwnd, int x, int y) {
    auto dpi = GetDpiForWindow(hwnd);
    return {MulDiv(x, dpi, 96), MulDiv(y, dpi, 96)};
}
POINT screen(HWND hwnd, POINT pt) {
    ClientToScreen(hwnd, &pt);
    return pt;
}
RECT bounds(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    return r;
}
HWND hold_card(HWND workspace, POINT grip) {
    send(workspace, WM_LBUTTONDOWN, {grip.x - 12, grip.y}, MK_LBUTTON);
    send(workspace, WM_MOUSEMOVE, grip, MK_LBUTTON);
    const auto visual = FindWindowW(L"ClipRope.DragCard", nullptr);
    require(visual && GetWindow(visual, GW_OWNER) == workspace && IsWindowVisible(visual),
            "Immediate move did not lift the card");
    return visual;
}
COLORREF color(unsigned rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

struct Painted {
    COLORREF background{};
    int first = -1, last = -1;
    unsigned ink_pixels{};
};
Painted read_paper(HWND hwnd, POINT bg, RECT text, unsigned rgb) {
    RECT size{};
    GetClientRect(hwnd, &size);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.right;
    info.bmiHeader.biHeight = -size.bottom;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biPlanes = 1;
    HDC dc = CreateCompatibleDC(nullptr);
    void *raw{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    require(dc && bitmap && raw, "Memory surface failed");
    auto previous = SelectObject(dc, bitmap);
    SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    auto *pixels = static_cast<DWORD *>(raw);
    Painted result;
    result.background = color(pixels[bg.y * size.right + bg.x]);
    for (LONG y = text.top; y < text.bottom && y < size.bottom; ++y)
        for (LONG x = text.left; x < text.right && x < size.right; ++x)
            if ((pixels[y * size.right + x] & 0xffffff) == rgb) {
                if (result.first < 0)
                    result.first = y;
                result.last = y;
                ++result.ink_pixels;
            }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return result;
}
Painted projected(HWND hwnd, unsigned text_color) {
    const auto bg = at(hwnd, 3, 67), top = at(hwnd, 12, 46), bottom = at(hwnd, 226, 150);
    return read_paper(hwnd, bg, {top.x, top.y, bottom.x, bottom.y}, text_color);
}
// A Hanging paper hangs from the rope's curve, so its probes follow the slot.
Painted projected(const win::Workspace &shelf, unsigned text_color) {
    const auto hwnd = shelf.window();
    RECT slot = shelf.slots().at(0).second;
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&slot), 2);
    const auto edge = at(hwnd, 3, 0), inset = at(hwnd, 8, 6);
    return read_paper(hwnd, {slot.left + edge.x, (slot.top + slot.bottom) / 2},
                      {slot.left + inset.x, slot.top + inset.y, slot.right - inset.x, slot.bottom - inset.y},
                      text_color);
}
void exercise() {
    application::CardSummary sample;
    sample.entry.id = "sample";
    sample.text = L"Saved text wraps here";
    sample.style.background_color = 0xf9e8b8;
    sample.style.text_color = 0x832036;
    sample.style.font_size = 12;
    int errors = 0, detaches = 0, reorders = 0, moves = 0, returns = 0, closes = 0, body_presses = 0, doubles = 0,
        contexts = 0;
    int anchor_writes=0,summary_reads=0;
    win::WorkspaceReads reads{[](auto) { return 1u; },
                              [&](auto, std::size_t offset, std::size_t count) {
                                  ++summary_reads;
                                  return offset || !count ? std::vector<application::CardSummary>{}
                                                          : std::vector<application::CardSummary>{sample};
                              },
                              [](auto, auto) { return std::optional<std::size_t>{0}; },
                              [](auto) { return application::Anchor{}; }, [&](auto, auto &) {++anchor_writes;}};
    presentation::ColorPreview colors;
    win::WorkspaceActions actions;
    actions.colors = &colors;
    actions.detach = [&](auto &, POINT, float, float) { ++detaches; };
    actions.reorder = [&](auto &id, std::size_t index, win::DropCompleted landed) {
        require(id == sample.entry.id && index == 0, "Drop changed the target slot");
        ++reorders;
        landed(id);
    };
    actions.error = [&] { ++errors; };
    win::Workspace shelf(std::move(reads), std::move(actions));
    require(shelf.open(), "Could not construct workspace");
    shelf.show("main");
    auto hwnd = shelf.window();
    const auto start = at(hwnd, 80, 140), outside = at(hwnd, 90, 300);
    send(hwnd, WM_LBUTTONDOWN, start, MK_LBUTTON);
    send(hwnd, WM_LBUTTONUP, start);
    require(!detaches && !reorders, "Stationary click committed a drag");
    const auto visual = hold_card(hwnd, start);
    const auto input = GetCapture();
    require(input && input != hwnd && GetWindow(input, GW_OWNER) == hwnd, "Drag input surface lacks capture");
    const auto before = bounds(visual);
    POINT destination = screen(hwnd, outside);
    ScreenToClient(input, &destination);
    send(input, WM_MOUSEMOVE, destination, MK_LBUTTON);
    const auto after = bounds(visual);
    if (after.left - before.left != outside.x - start.x || after.top - before.top != outside.y - start.y)
        std::cerr << "Preview delta " << after.left - before.left << "," << after.top - before.top
                  << " for pointer delta " << outside.x - start.x << "," << outside.y - start.y << '\n';
    require(after.left - before.left == outside.x - start.x && after.top - before.top == outside.y - start.y &&
            after.bottom > bounds(hwnd).bottom, "Rapid move clipped the preview or displaced its grip");
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    send(hwnd, WM_LBUTTONUP, outside);
    require(!detaches && !IsWindow(visual) && !GetCapture(), "Escape left a preview or detached a card");
    const auto stolen = hold_card(hwnd, start);
    SetCapture(hwnd);
    send(hwnd, WM_LBUTTONUP, outside);
    require(!detaches && !IsWindow(stolen), "Losing capture committed or retained the drag");
    const auto landing = hold_card(hwnd, start);
    const auto lifted = bounds(landing);
    const auto off_center = at(hwnd, 150, 145);
    send(hwnd, WM_MOUSEMOVE, off_center, MK_LBUTTON);
    const auto carried = bounds(landing);
    send(hwnd, WM_LBUTTONUP, off_center);
    require(reorders == 1 && IsWindowVisible(landing) && !GetCapture(), "Drop skipped the landing transition");
    bool intermediate = false;
    const auto deadline = GetTickCount64() + 1500;
    while (IsWindow(landing) && GetTickCount64() < deadline) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        const auto x = bounds(landing).left;
        intermediate |= IsWindow(landing) && x > lifted.left + 3 && x < carried.left - 3;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    require(intermediate && !IsWindow(landing), "Released card did not travel back to the rope");
    hold_card(hwnd, start);
    send(hwnd, WM_MOUSEMOVE, outside, MK_LBUTTON);
    send(hwnd, WM_LBUTTONUP, outside);
    require(detaches == 1, "Drag-out did not detach");

    const win::FloatingMonitor left{{-1920, -120, 0, 960}, "left", 144};
    const win::FloatingMonitor right{{0, 40, 1920, 1080}, "right", 96};
    const auto left_note = left.centered_text_layout(), right_note = right.centered_text_layout();
    require(left_note.monitor == left.name && left_note.x == 520 && left_note.y == 280 &&
            right_note.monitor == right.name && right_note.x == 840 && right_note.y == 440 &&
            left_note.width == 240 && right_note.height == 160, "Mixed-DPI quick note placement used the wrong work area");
    const auto layout = win::floating_layout_at(screen(hwnd, outside));
    const domain::Layout lost{"absent monitor", 1e8, -1e8, 240, 160};
    const auto rescued = win::resolve_floating(lost);
    MONITORINFO monitor{sizeof(monitor)};
    require(GetMonitorInfoW(MonitorFromRect(&rescued, MONITOR_DEFAULTTONULL), &monitor) != 0,
            "Detached monitor was not rescued");
    require(rescued.left >= monitor.rcWork.left && rescued.right <= monitor.rcWork.right &&
                rescued.top >= monitor.rcWork.top && rescued.bottom <= monitor.rcWork.bottom,
            "Restored card still off-screen");
    bool move_failure = false;
    win::FloatingActions fa;
    fa.colors = &colors;
    fa.press = [&](auto &) { ++body_presses; };
    fa.double_click = [&](auto &) { ++doubles; };
    fa.return_current = [&](auto &) { ++returns; };
    fa.close_card = [&](auto &) { ++closes; };
    fa.context = [&](const auto&, POINT) { ++contexts; };
    fa.preview_at = [&](POINT p, const auto& card) { return shelf.preview_floating(p, &card); };
    fa.clear_preview = [&] { shelf.clear_floating_preview(); };
    fa.drop = [&](auto &, const auto &target) {
        require(target.rope.id == "main", "Drop target mismatch");
        ++returns;
    };
    fa.moved = [&](auto &, const auto &) {
        if (move_failure)
            throw std::runtime_error("Injected placement failure");
        ++moves;
    };
    fa.error = [&] { ++errors; };
    win::Floating card(std::move(fa));
    require(card.open(sample, layout, false), "Could not create Floating candidate");
    auto floating = card.window();
    require(GetMonitorInfoW(MonitorFromWindow(floating, MONITOR_DEFAULTTONEAREST), &monitor),
            "Could not inspect the source monitor");
    const auto cascade = win::tile_file_group(floating, {monitor.rcWork}, 3, sample.image_preferences.size, true);
    require(cascade.size() == 3, "Missing staggered children");
    const auto r0 = win::resolve_floating(cascade[0]), r1 = win::resolve_floating(cascade[1]);
    RECT inside{};
    require(!EqualRect(&r0, &r1) && IntersectRect(&inside, &r1, &monitor.rcWork) &&
            EqualRect(&inside, &r1), "A full work area did not produce staggered on-screen children");
    require(!(GetWindowLongPtrW(floating, GWL_STYLE) & WS_VISIBLE), "Candidate was visible before commit");
    card.show();
    auto ex = GetWindowLongPtrW(floating, GWL_EXSTYLE);
    require((ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW),
            "Incorrect Floating window flags");
    auto body = at(floating, 135, 83);
    click(floating, at(floating, 35, 15));
    click(floating, at(floating, 220, 16));
    require(returns == 1 && closes == 1 && !body_presses && !doubles, "Controls dispatched a body gesture");
    click(floating, body);
    send(floating, WM_LBUTTONDBLCLK, body, MK_LBUTTON);
    send(floating, WM_LBUTTONUP, body);
    require(body_presses == 1 && doubles == 1, "Floating body binding or double-click failed");
    send(floating, WM_CONTEXTMENU, screen(floating, body), reinterpret_cast<WPARAM>(floating));
    require(contexts == 1, "Floating context was not routed");
    const RECT original = bounds(floating);
    move_failure = true;
    send(floating, WM_LBUTTONDOWN, body, MK_LBUTTON);
    auto shifted = body;
    shifted.x += 60;
    send(floating, WM_MOUSEMOVE, shifted, MK_LBUTTON);
    send(floating, WM_LBUTTONUP, body);
    const RECT reverted = bounds(floating);
    require(EqualRect(&original, &reverted) && errors == 1 && !moves,
            "Failed move did not restore placement");
    move_failure = false;

    // Move the same captured window into the first slot and back out, all
    // before button-up. Its sheet should change templates on each crossing.
    const auto rest = shelf.slots().front().second;
    const auto move_to = [&](POINT p) {
        ScreenToClient(floating, &p);
        send(floating, WM_MOUSEMOVE, p, MK_LBUTTON);
    };
    send(floating, WM_LBUTTONDOWN, body, MK_LBUTTON);
    move_to({rest.left + 8, (rest.top + rest.bottom) / 2});
    require(shelf.slots().front().second.left > rest.right && bounds(floating).bottom - bounds(floating).top ==
                at(floating, 0, 240).y, "Entering the rope did not open a numbered slot");
    const auto wood = read_paper(floating, at(floating, 118, 60), {}, 0).background;
    require(wood != color(sample.style.paper_rgb()) && GetRValue(wood) > GetBValue(wood) + 28,
            "Floating preview lacks its wooden hanger");
    move_to({original.left + body.x, original.top + body.y});
    require(shelf.slots().front().second.left == rest.left &&
                projected(floating, sample.style.ink_rgb()).background == color(sample.style.paper_rgb()),
            "Leaving the rope retained the hanger or insertion gap");
    SendMessageW(floating, WM_KEYDOWN, VK_ESCAPE, 0);
    require(!moves && returns == 1, "Preview crossing committed a drop or window move");

    const auto small_h = projected(shelf, sample.style.ink_rgb());
    const auto small_f = projected(floating, sample.style.ink_rgb());
    require(small_h.background == color(sample.style.paper_rgb()) &&
                small_f.background == small_h.background && small_h.first >= 0 && small_f.first >= 0,
            "Initial style absent from a renderer");
    const auto original_style = sample.style;
    const auto original_reads=summary_reads,original_writes=anchor_writes;
#ifdef CLIPROPE_DRAW_PROBES
    const auto shared_paint = rendering::WorkspacePaint::acquire();
    const auto face = [&](const presentation::ColorPreview* view) {
        win::MenuBitmap target;
        constexpr int width = 344, height = 280;
        require(target.create(width,height), "Cannot allocate carried-face fixture");
        domain::Rope rope;
        rope.id = "main";
        const auto geometry = presentation::ImageCardLayout::measure(sample);
        require(shared_paint->draw_carried(target.dc,{0,0,width,height},96,sample,rope,0,geometry,
                                           {float(width)/2,24,0},view), "Could not draw the carried face");
        GdiFlush();
        return std::vector<std::uint32_t>(target.pixels, target.pixels + width*height);
    };
    const auto capture = [&](const char* phase) {
        if (color_output.empty()) return;
        workspace_fixture::frame(shelf.window(),color_output / (std::string("workspace-")+phase+".bmp"));
        workspace_fixture::frame(floating,color_output / (std::string("floating-")+phase+".bmp"),false);
    };
    const auto old_face = face(&colors);
    capture("original");
#endif
    colors.card =
        presentation::CardColorPreview{sample.entry.id, true, domain::ColorRGBA8{255, 0, 0, 128}};
    shelf.repaint_colors();
    InvalidateRect(floating,nullptr,FALSE);
    const auto tinted = domain::composite(domain::ColorRGBA8{255, 0, 0, 128}, domain::paper_default);
    require(projected(shelf, sample.style.ink_rgb()).background == color(tinted) &&
                projected(floating, sample.style.ink_rgb()).background == color(tinted) &&
                sample.style == original_style,
            "Tint preview missed a surface or mutated its read model");
#ifdef CLIPROPE_DRAW_PROBES
    require(face(&colors) != old_face, "Moving face reused the previous style");
    capture("preview");
#endif
    colors.card.reset();
    shelf.repaint_colors();
    InvalidateRect(floating,nullptr,FALSE);
    require(projected(shelf, sample.style.ink_rgb()).background == small_h.background &&
                projected(floating, sample.style.ink_rgb()).background == small_f.background,
            "Cancel did not restore both surfaces");
    require(summary_reads==original_reads&&anchor_writes==original_writes,"Color preview reloaded history or wrote an anchor");
#ifdef CLIPROPE_DRAW_PROBES
    require(face(&colors) == old_face, "Moving face kept the canceled style");
    capture("cancel");
#endif
    sample.style.bold = true;
    shelf.refresh();
    card.update(sample);
    require(projected(shelf, sample.style.ink_rgb()).ink_pixels > small_h.ink_pixels &&
                projected(floating, sample.style.ink_rgb()).ink_pixels > small_f.ink_pixels,
            "Bold text failed to increase the inked area at a fixed size");
    sample.style.font_size = 30;
    sample.style.background_color = 0xcef2dc;
    sample.style.text_color = 0x175944;
    shelf.refresh();
    card.update(sample);
    const auto large_h = projected(shelf, sample.style.ink_rgb());
    const auto large_f = projected(floating, sample.style.ink_rgb());
    require(large_h.background == color(sample.style.paper_rgb()) &&
                large_f.background == large_h.background &&
                large_h.last - large_h.first > small_h.last - small_h.first + 8 &&
                large_f.last - large_f.first > small_f.last - small_f.first + 8,
            "Style change did not reflow both texts");
    sample.style.text_color = 0x864cbc;
    shelf.refresh();
    card.update(sample);
    auto recolor_h = projected(shelf, sample.style.ink_rgb()),
         recolor_f = projected(floating, sample.style.ink_rgb());
    require(recolor_h.first == large_h.first && recolor_h.last == large_h.last &&
                recolor_f.first == large_f.first && recolor_f.last == large_f.last,
            "Color-only repaint changed layout");
    const auto paper = screen(hwnd, at(hwnd, 38, 145)), floating_paper = screen(floating, body);
    for (double opacity : {0.65, 0., 1.}) {
        sample.style.opacity = opacity;
        shelf.refresh();
        card.update(sample);
        UpdateWindow(hwnd);
        UpdateWindow(floating);
        // The workspace always composes per-pixel alpha; a Floating sheet only while translucent.
        require((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) &&
                    bool(GetWindowLongPtrW(floating, GWL_EXSTYLE) & WS_EX_LAYERED) == (opacity < 1),
                "Opacity not applied to native surface");
        const bool hanging_hit = WindowFromPoint(paper) == hwnd;
        const bool floating_hit = WindowFromPoint(floating_paper) == floating;
        if (hanging_hit != (opacity != 0) || floating_hit != (opacity != 0)) {
            std::cerr << "Opacity " << opacity << ", Hanging hit " << hanging_hit
                      << ", Floating hit " << floating_hit << ", callback errors " << errors << '\n';
            throw std::runtime_error("Per-pixel alpha did not change desktop hit mask");
        }
        require(WindowFromPoint(screen(floating, at(floating, 35, 15))) == floating,
                "Transparent card lost its return control");
    }
    sample.kind = application::CardKind::image;
    sample.source = {1080,1920};
    sample.style.opacity = 1;
    card.update(sample);
    const auto portrait = bounds(floating);
    const auto expected = at(floating, 144, 182); // The toolbar is wider than the 81-DIP portrait.
    require(std::abs(portrait.right - portrait.left - expected.x) <= 2 &&
            std::abs(portrait.bottom - portrait.top - expected.y) <= 1, "Floating portrait did not fit its source");
    shelf.refresh();
    SendMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::density), 0);
    const auto image_grip = at(floating, 100, 100);
    send(floating, WM_LBUTTONDOWN, image_grip, MK_LBUTTON);
    const auto row = shelf.rows().front().second;
    const POINT image_target{row.left + at(hwnd, 300, 0).x, (row.top + row.bottom) / 2};
    move_to(image_target);
    const auto preview_image = bounds(floating);
    const auto base = presentation::ImageCardLayout::measure(sample);
    const auto dense = presentation::ImageCardLayout::measure(sample, 86, true);
    const float gx = (float(image_grip.x) / (portrait.right - portrait.left) - .5f) * base.float_width() / 2;
    const float gy = dense.top + (float(image_grip.y) / (portrait.bottom - portrait.top) *
                                  (base.toolbar + base.height()) - base.toolbar) / 2;
    const auto pixels = [&](float dip) { return int(std::lround(dip * GetDpiForWindow(floating) / 96)); };
    require(std::abs(preview_image.left - (image_target.x - pixels(dense.width / 2 + dense.carried_padding() + gx))) <= 1 &&
            std::abs(preview_image.top - (image_target.y - pixels(24 + gy))) <= 1 &&
            preview_image.right - preview_image.left == pixels(dense.width + 2 * dense.carried_padding()),
            "Compact Floating image preview changed the gripped point or width");
    SendMessageW(floating, WM_KEYDOWN, VK_ESCAPE, 0);
    const auto restored_portrait = bounds(floating);
    require(EqualRect(&portrait, &restored_portrait), "Canceling compact image preview lost Floating placement");
    sample.source = {2,2};
    card.update(sample);
    const auto small = bounds(floating);
    const auto minimum = at(floating, 144, 41);
    require(std::abs(small.right - small.left - minimum.x) <= 1 &&
            std::abs(small.bottom - small.top - minimum.y) <= 1 &&
            small.left == portrait.left && small.top == portrait.top, "Tiny image lost its toolbar or saved origin");
    const auto old_returns = returns;
    click(floating, at(floating, 20, 15));
    require(returns == old_returns + 1, "Tiny image could not return from its toolbar");
    require(errors == 1, "A rendering or input callback failed");
    std::cout << "Two native surfaces: font/weight reflow, color-only repaint and alpha 0.65/0/1 verified. "
                 "Staged detach, lost-monitor rescue, rollback, return/recycle/body/context verified.\n";
}
void resize_surfaces() {
    application::CardSummary card;
    card.entry.id = "resized";
    card.text = L"Resize sample with wrapped text";
    domain::CardScale stored;
    win::WorkspaceReads reads;
    reads.count = [](auto) { return 3u; };
    reads.page = [&](auto rope, size_t offset, size_t count) {
        std::vector<application::CardSummary> result;
        for (size_t i = offset; i < offset + count; ++i) {
            auto c = card;
            c.entry.rope_id = rope;
            if (rope != "main" || i != 1) c.entry.id = std::string(rope) + std::to_string(i);
            else c.scale = stored;
            result.push_back(c);
        }
        return result;
    };
    reads.position = [&](auto rope, auto id) -> std::optional<size_t> {
        if (id == card.entry.id && rope == "main") return 1;
        for (size_t i = 0; i < 3; ++i)
            if (id == std::string(rope) + std::to_string(i)) return i;
        return {};
    };
    reads.load_anchor = [](auto) { return application::Anchor{}; };
    reads.save_anchor = [](auto, const auto&) {};
    reads.rope_count = [] { return 2u; };
    reads.ropes = [](size_t offset, size_t count) {
        const std::vector<domain::Rope> ropes{{"main","main"}, {"next","Next"}};
        return std::vector<domain::Rope>(ropes.begin() + offset, ropes.begin() + offset + count);
    };
    reads.rope_position = [](auto rope) { return std::optional<size_t>{rope == "main" ? 0u : 1u}; };
    int writes = 0, errors = 0, recalls = 0;
    bool reject = false;
    auto save = [&](const domain::Entry& entry, domain::CardScale size, const domain::Layout* placement) {
        require(entry.id == card.entry.id && bool(entry.rope_id) != bool(placement), "Resize changed its bound target");
        if (reject) throw std::runtime_error("Injected resize failure");
        stored = size;
        ++writes;
    };
    win::WorkspaceActions actions;
    actions.resize = save;
    actions.press = [](const auto&) {};
    actions.double_click = [&](const auto&) { ++recalls; };
    actions.error = [&] { ++errors; };
    win::Workspace shelf(std::move(reads), std::move(actions));
    require(shelf.open(), "Resize workspace unavailable");
    shelf.show("main");
    const auto hwnd = shelf.window();
    const auto seq = GetClipboardSequenceNumber();
    auto slot = [&] {
        for (const auto& [id, r] : shelf.slots()) if (id == card.entry.id) return r;
        throw std::runtime_error("Resized card disappeared");
    };
    auto pointer = [](HWND window, UINT msg, POINT p) {
        ScreenToClient(window, &p);
        send(window, msg, p, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON);
    };
    for (const bool overview : {false, true}) {
        if (overview) SendMessageW(hwnd, WM_COMMAND, WPARAM(win::WorkspaceCommand::overview), 0);
        const auto original = slot();
        const auto delta = at(hwnd, 24, 30);
        const POINT start{original.right - 2, original.bottom - 2};
        const POINT end{start.x + delta.x, start.y + delta.y};
        for (int attempt = 0; attempt < 4; ++attempt) {
            const int previous = writes;
            reject = attempt == 2;
            pointer(hwnd, WM_LBUTTONDOWN, start);
            const auto input = GetCapture();
            require(input && input != hwnd, "Resize edge did not capture input");
            pointer(input, WM_MOUSEMOVE, end);
            const auto resized = slot();
            require(std::abs(resized.right - original.right - delta.x) <= 1 &&
                    std::abs(original.left - resized.left - delta.x) <= 1 &&
                    std::abs(resized.bottom - original.bottom - delta.y) <= 1 &&
                    resized.top == original.top && writes == previous, "Hanging preview moved its hook or wrote early");
            if (!attempt) SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            else if (attempt == 1) ReleaseCapture();
            else pointer(input, WM_LBUTTONUP, end);
            const auto after = slot();
            require(attempt == 3 ? EqualRect(&resized, &after) && writes == previous + 1 :
                                   EqualRect(&original, &after) && writes == previous,
                    "Hanging border release or cancellation lost the preview rectangle");
        }
        require(WindowFromPoint({slot().right + at(hwnd, 12, 0).x, slot().bottom - 8}) != hwnd,
                "Resized transparent gap intercepted desktop input");
    }
    // Even at its smallest the image must respond to a stationary double-click on paper.
    card.kind = application::CardKind::image;
    card.source = {2, 2};
    shelf.refresh();
    const auto tiny = slot();
    const POINT body{(tiny.left + tiny.right) / 2, (tiny.top + tiny.bottom) / 2};
    pointer(hwnd, WM_LBUTTONDOWN, body);
    pointer(hwnd, WM_LBUTTONUP, body);
    pointer(hwnd, WM_LBUTTONDBLCLK, body);
    pointer(hwnd, WM_LBUTTONUP, body);
    require(recalls == 1 && writes == 2, "Tiny image center lost its double-click");
    shelf.hide();
    card.kind = application::CardKind::text;
    reject = false;
    win::FloatingActions fa;
    fa.resize = save;
    fa.error = [&] { ++errors; };
    win::Floating floating(std::move(fa));
    card.scale = stored;
    require(floating.open(card, {"", 100, 150, 240, 160}), "Resized Floating window unavailable");
    HWND window = floating.window();
    const RECT original = bounds(window);
    const auto delta = at(window, 20, 14);
    const POINT start{original.left + 2, original.top + 2};
    const POINT end{start.x - delta.x, start.y - delta.y};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int previous = writes;
        reject = attempt == 2;
        pointer(window, WM_LBUTTONDOWN, start);
        pointer(window, WM_MOUSEMOVE, end);
        const auto resized = bounds(window);
        require(resized.right == original.right && resized.bottom == original.bottom &&
                std::abs(resized.left - original.left + delta.x) <= 1 && std::abs(resized.top - original.top + delta.y) <= 1 &&
                writes == previous, "Floating corner moved its opposite corner or wrote early");
        if (!attempt) SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        else if (attempt == 1) ReleaseCapture();
        else pointer(window, WM_LBUTTONUP, end);
        const auto after = bounds(window);
        require(attempt == 3 ? EqualRect(&resized, &after) && writes == previous + 1 :
                               EqualRect(&original, &after) && writes == previous,
                "Floating resize cancellation or commit lost its rectangle");
    }
    const auto committed = bounds(window);
    card.kind = application::CardKind::image;
    card.source = {90, 300};
    card.scale = {1, 1};
    floating.update(card);
    const auto portrait = bounds(window);
    const auto portrait_start = POINT{(portrait.left + portrait.right) / 2, portrait.bottom - 2};
    const auto portrait_end = POINT{portrait_start.x, portrait_start.y + at(window, 0, 72).y};
    reject = false;
    pointer(window, WM_LBUTTONDOWN, portrait_start);
    pointer(window, WM_MOUSEMOVE, portrait_end);
    const auto preview = bounds(window);
    require(preview.left == portrait.left && preview.right == portrait.right && preview.top == portrait.top &&
            std::abs(preview.bottom - portrait.bottom - at(window, 0, 72).y) <= 1,
            "Portrait resize displaced its minimum-width toolbar");
    pointer(window, WM_LBUTTONUP, portrait_end);
    require(stored.width == stored.height && std::abs(stored.width - 1.5f) < .01f,
            "Portrait border did not commit a shared uniform scale");
    card.scale = stored;
    floating.update(card);
    const auto updated = bounds(window);
    require(EqualRect(&preview, &updated) && !EqualRect(&committed, &preview), "Late image update discarded its scale");
    require(writes == 4 && errors == 3 && seq == GetClipboardSequenceNumber(),
            "Resize changed the clipboard or dispatched an unexpected action");
}

#ifdef CLIPROPE_DRAW_PROBES
template<class Paint>
void time_surface(const char* surface, const char* stage, int trial, Paint paint) {
    const auto process = GetCurrentProcess();
    const auto before_targets = rendering::WorkspacePaint::target_creations();
    const auto before_gdi = GetGuiResources(process, GR_GDIOBJECTS);
    const auto before_user = GetGuiResources(process, GR_USEROBJECTS);
    const auto start = std::chrono::steady_clock::now();
    paint();
    const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "surface=" << surface << " stage=" << stage << " trial=" << trial << " app-ms=" << milliseconds
              << " targets=" << before_targets << "->" << rendering::WorkspacePaint::target_creations()
              << " GDI=" << before_gdi << "->" << GetGuiResources(process, GR_GDIOBJECTS)
              << " USER=" << before_user << "->" << GetGuiResources(process, GR_USEROBJECTS) << '\n';
}

void close_frames(const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    std::vector<application::CardSummary> cards;
    for (int i = 1; i <= 3; ++i) {
        application::CardSummary card;
        card.entry.id = std::to_string(i);
        card.entry.rope_id = "main";
        card.text = L"Rapid note 0" + std::to_wstring(i);
        cards.push_back(card);
    }
    win::WorkspaceReads reads{
        [&](auto) { return cards.size(); },
        [&](auto, size_t offset, size_t count) {
            const auto begin = cards.begin() + std::min(offset, cards.size());
            const auto end = begin + std::min(count, size_t(cards.end() - begin));
            return std::vector<application::CardSummary>(begin, end);
        },
        [&](auto, std::string_view id) -> std::optional<size_t> {
            for (size_t i = 0; i < cards.size(); ++i) if (cards[i].entry.id == id) return i;
            return {};
        }, [](auto) { return application::Anchor{}; }, [](auto, auto&) {}};
    std::vector<domain::Id> erased;
    win::WorkspaceActions actions;
    actions.erase = [&](const auto& id, const auto& rope) {
        require(rope == "main" && !cards.empty() && id == cards.front().entry.id, "Close changed its rope or slot");
        erased.push_back(id);
        cards.erase(cards.begin());
    };
    win::Workspace shelf(std::move(reads), std::move(actions));
    require(shelf.open(), "Could not open rapid-close workspace");
    shelf.show("main");
    const auto workspace = shelf.window();
    const auto slot = shelf.slots().at(0).second;
    const auto inset = at(workspace, 13, 12);
    POINT close{slot.right - inset.x, slot.top + inset.y};
    require(WindowFromPoint(close) == workspace, "Hanging close had an unexpected HWND");
    ScreenToClient(workspace, &close);
    workspace_fixture::frame(workspace, output / "hanging-start.bmp");
    for (int click_index = 0; click_index < 2; ++click_index) {
        const auto previous_count = erased.size();
        time_surface("workspace", "rapid-close-and-refresh", click_index + 1, [&] {
            send(workspace, click_index ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, close, MK_LBUTTON);
            require(GetCapture() == workspace, "Hanging close did not capture its own HWND");
            send(workspace, WM_LBUTTONUP, close);
            require(!GetCapture() && erased.size() == previous_count + 1, "Hanging release did not commit once");
            shelf.refresh();
        });
        workspace_fixture::frame(workspace, output / (click_index ? "hanging-after.bmp" : "hanging-first.bmp"));
    }
    require(erased == std::vector<domain::Id>{"1", "2"} && shelf.slots().at(0).first == "3",
            "The last Hanging entry did not become the first displayed card");

    auto floating_card = cards.front();
    floating_card.entry.id = "floating";
    floating_card.entry.rope_id.reset();
    floating_card.text = L"Independent note";
    int attempts{}, errors{}, edits{}, writes{};
    bool reject = true;
    win::InlineEditorHost inline_editor;
    win::Floating* opened{};
    win::FloatingActions floating_actions;
    floating_actions.editor = &inline_editor;
    floating_actions.save_text = [&](const auto&, std::wstring_view) {
        ++writes;
        return domain::Id{"unexpected-save"};
    };
    floating_actions.close_card = [&](const auto& id) {
        require(id == "floating", "Floating close changed its target");
        ++attempts;
        if (reject) throw std::runtime_error("Injected close rejection");
    };
    floating_actions.error = [&] { ++errors; };
    floating_actions.double_click = [&](const auto& id) {
        require(id == floating_card.entry.id, "Floating editor received another entry");
        ++edits;
        opened->edit_text({id, floating_card.entry.revision, floating_card.text});
    };
    win::Floating floating(std::move(floating_actions));
    opened = &floating;
    require(floating.open(floating_card, win::floating_layout_at(screen(workspace, at(workspace, 400, 350)))),
            "Floating rapid-close fixture unavailable");
    const auto hwnd = floating.window();
    const auto rect = bounds(hwnd);
    const auto corner = at(hwnd, 19, 19);
    POINT x{rect.right - rect.left - corner.x, corner.y};
    require(WindowFromPoint(screen(hwnd, x)) == hwnd, "Floating close had an unexpected HWND");
    workspace_fixture::frame(hwnd, output / "floating-start.bmp", false);
    click(hwnd, x);
    require(attempts == 1 && errors == 1 && IsWindowVisible(hwnd), "Rejected close lost the Floating window");
    workspace_fixture::frame(hwnd, output / "floating-rejected.bmp", false);
    const auto body = at(hwnd, 100, 80);
    send(hwnd, WM_LBUTTONDBLCLK, x, MK_LBUTTON);
    require(GetCapture() == hwnd, "Floating repeated close did not capture its own HWND");
    send(hwnd, WM_LBUTTONUP, body);
    send(hwnd, WM_LBUTTONDBLCLK, x, MK_LBUTTON);
    SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
    send(hwnd, WM_LBUTTONUP, x);
    require(attempts == 1 && !GetCapture(), "Off-target or canceled release repeated the close");
    click(hwnd, body);
    send(hwnd, WM_LBUTTONDBLCLK, body, MK_LBUTTON);
    send(hwnd, WM_LBUTTONUP, body);
    require(edits == 1 && attempts == 1, "Editable body did not receive its original double-click");
    const auto editor_hwnd = FindWindowW(L"ClipRope.InlineEditorHost", nullptr);
    const auto field = GetDlgItem(editor_hwnd, 1760);
    require(inline_editor.owner() == hwnd && editor_hwnd && GetWindow(editor_hwnd, GW_OWNER) == hwnd &&
                IsWindowVisible(editor_hwnd) && field && win::fluent::caption(field) == floating_card.text,
            "Floating body did not bind the real visible editor and original draft");
    require(SetWindowTextW(field, L"Discard this fixture draft") != FALSE, "Cannot edit the native draft");
    SendMessageW(editor_hwnd, WM_COMMAND, IDCANCEL, 0);
    const auto deadline = GetTickCount64() + 2000;
    while (inline_editor.owner() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        if (inline_editor.owner()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    require(!inline_editor.owner() && !GetWindow(editor_hwnd, GW_OWNER) && !IsWindowVisible(editor_hwnd) &&
                !writes && attempts == 1, "Cancelled Rich Edit draft did not return to its actual Floating owner");
    workspace_fixture::frame(hwnd, output / "floating-cancelled.bmp", false);
    reject = false;
    time_surface("floating", "repeated-close", 1, [&] {
        send(hwnd, WM_LBUTTONDBLCLK, x, MK_LBUTTON);
        send(hwnd, WM_LBUTTONUP, x);
    });
    require(attempts == 2 && errors == 1 && !GetCapture(), "Retry did not deliver precisely one successful close");
    std::cout << "Hanging recipients=1,2 survivor=3; Floating close attempts=2 rejected=1 canceled=1 body=1 "
                 "native-editor=cancelled writes=0\n";
}

void recover_editor_frame(const std::filesystem::path& output) {
    win::InlineEditorHost editor;
    unsigned writes{}, completions{};
    win::FloatingActions actions;
    actions.editor = &editor;
    actions.save_text = [&](const application::TextDraft&, std::wstring_view text) -> domain::Id {
        require(text == L"Unsaved draft", "Recovered editor wrote a different draft");
        ++writes;
        return "new-snapshot";
    };
    application::CardSummary model;
    model.entry.id = "floating";
    model.text = L"Original";
    model.style.opacity = .9;
    win::Floating window(std::move(actions));
    require(window.open(model, {"", 50, 150, 240, 160}), "Recovery card failed to open");
    window.edit_text({model.entry.id, model.entry.revision, model.text});
    const auto host = FindWindowW(L"ClipRope.InlineEditorHost", nullptr);
    const auto field = GetDlgItem(host, 1760);
    require(field && SetWindowTextW(field, L"Unsaved draft"), "Could not set the recovery draft");
    auto wait_for_edit = [&] {
        const auto deadline = GetTickCount64() + 3000;
        while (editor.previewing() && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            if (editor.previewing()) MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        require(!editor.previewing(), "Persistent presentation fault left the editor waiting");
    };
    std::filesystem::create_directories(output);
    management_fixture::record_frames = true;
    management_fixture::frame(host, output / "editor-before.bmp");
    for (bool cancel : {false, true}) {
        rendering::inject_draw_fault(rendering::DrawFault::present_layer);
        if (cancel) editor.cancel();
        else editor.save([&] { ++completions; });
        wait_for_edit();
        rendering::inject_draw_fault(rendering::DrawFault::none);
        require(editor.owner() == window.window() && win::fluent::caption(field) == L"Unsaved draft" &&
                !(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY) &&
                !win::fluent::caption(GetDlgItem(host, 1761)).empty() && !writes && !completions,
                "Failed presentation changed or froze the draft");
        workspace_fixture::frame(window.window(), output / (cancel ? "cancel-failed.bmp" : "save-failed.bmp"), false);
        management_fixture::frame(host, output / (cancel ? "editor-cancel-error.bmp" : "editor-save-error.bmp"));
    }
    // Recover the target during the retry interval without submitting a second action.
    rendering::inject_draw_fault(rendering::DrawFault::present_layer);
    editor.save([&] { ++completions; });
    rendering::inject_draw_fault(rendering::DrawFault::none);
    wait_for_edit();
    require(!editor.owner() && writes == 1 && completions == 1, "Transient fault lost or duplicated the retry");
    workspace_fixture::frame(window.window(), output / "saved.bmp", false);
}

void recover_draw(const std::filesystem::path& output) {
    using rendering::DrawFault;
    application::CardSummary model;
    model.entry.id = "hanging";
    model.text = L"Recovery sample";
    model.style.opacity = .9;
    win::WorkspaceReads reads{[](auto) { return 1u; },
        [&](auto, size_t offset, size_t count) {
            return !offset && count ? std::vector{model} : std::vector<application::CardSummary>{};
        }, [](auto, auto) { return std::optional<size_t>{0}; },
        [](auto) { return application::Anchor{}; }, [](auto, auto&) {}};
    std::string pressed;
    unsigned layouts{};
    win::WorkspaceActions actions;
    actions.press = [&](const auto& id) { pressed = id; };
    actions.layout_changed = [&] { ++layouts; };
    win::Workspace shelf(std::move(reads), std::move(actions));
    require(shelf.open(), "Recovery workspace unavailable");
    const auto initial_frames = shelf.painted_frames();
    time_surface("workspace", "show", 0, [&] { shelf.show("main"); });
    require(shelf.painted_frames() > initial_frames, "First workspace show did not present");
    auto hwnd = shelf.window();
    auto floating_model = model;
    floating_model.entry.id = "floating";
    win::FloatingActions floating_actions;
    floating_actions.press = [&](const auto& id) { pressed = id; };
    win::Floating floating(std::move(floating_actions));
    require(floating.open(floating_model, win::floating_layout_at(screen(hwnd, at(hwnd, 400, 350))), false),
            "Recovery Floating window unavailable");
    const auto card = floating.window();
    time_surface("floating", "show", 0, [&] { floating.show(); });
    auto redraw = [](HWND surface) {
        require(RedrawWindow(surface, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW), "Could not redraw native surface");
    };
    for (int i = 0; i < 5; ++i) {
        time_surface("workspace", "repeat", i, [&] { redraw(hwnd); });
        time_surface("floating", "repeat", i, [&] { redraw(card); });
    }
    std::filesystem::create_directories(output);
    workspace_fixture::frame(hwnd, output / "workspace-before.bmp");
    workspace_fixture::frame(card, output / "floating-before.bmp", false);
    const auto focus = GetFocus();
    const auto start_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto start_user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (auto fault : {DrawFault::create_tile, DrawFault::finish_tile, DrawFault::finish_outer, DrawFault::present_layer}) {
        const auto before = shelf.painted_frames();
        const auto accepted_layouts = layouts;
        rendering::WorkspacePaint::discard_target();
        rendering::inject_draw_fault(fault);
        try { shelf.refresh(); } catch (const std::runtime_error&) {}
        InvalidateRect(card, nullptr, FALSE);
        UpdateWindow(card);
        // Drain the one scheduled retry while the same fault is still present.
        UpdateWindow(hwnd);
        UpdateWindow(card);
        require(!GetUpdateRect(hwnd, nullptr, FALSE) && !GetUpdateRect(card, nullptr, FALSE),
                "Persistent fault kept posting paint requests");
        require(before == shelf.painted_frames() && layouts == accepted_layouts, "Failed frame acknowledged a layout");
        const auto slot = shelf.slots().at(0).second;
        POINT point{(slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2};
        const auto body = at(card, 100, 72);
        require(WindowFromPoint(point) == hwnd && WindowFromPoint(screen(card, body)) == card, "Wrong hit HWND");
        ScreenToClient(hwnd, &point);
        pressed.clear();
        click(hwnd, point);
        click(card, body);
        require(pressed.empty(), "Failed surface accepted an entry press");
        rendering::inject_draw_fault(DrawFault::none);
        // The second window must be able to use the shared target first.
        time_surface("floating", "recovery", int(fault), [&] { redraw(card); });
        time_surface("workspace", "recovery", int(fault), [&] { redraw(hwnd); });
        click(card, body);
        require(pressed == "floating", "Second surface did not recover its entry hit");
        require(shelf.painted_frames() > before && layouts == accepted_layouts + 1, "Workspace recovery lost its layout");
        click(hwnd, point);
        require(pressed == "hanging" && GetFocus() == focus, "Recovery changed input recipient or focus");
    }
    workspace_fixture::frame(hwnd, output / "workspace-after.bmp");
    workspace_fixture::frame(card, output / "floating-after.bmp", false);
    const auto gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    require(gdi <= start_gdi + 4 && user <= start_user + 2, "Recovery retained native resources");
    std::cout << "failure-points=4 surfaces=2 recovered-first=Floating GDI=" << start_gdi << "->" << gdi
              << " USER=" << start_user << "->" << user << '\n';
}
#endif

} // namespace
int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto name = L"ClipRope.FloatingTest." + std::to_wstring(GetCurrentProcessId());
    auto previous = GetThreadDesktop(GetCurrentThreadId());
    HDESK private_desk = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!private_desk || !SetThreadDesktop(private_desk)) {
        std::cerr << "Private desktop unavailable\n";
        return 1;
    }
    int status = 0;
    try {
#ifdef CLIPROPE_DRAW_PROBES
        if (argc == 3 && std::string_view(argv[1]) == "--draw-recovery") recover_draw(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--editor-recovery") recover_editor_frame(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--color-frames") {
            color_output = argv[2];
            std::filesystem::create_directories(color_output);
            exercise();
        }
        else if (argc == 3 && std::string_view(argv[1]) == "--close-frames") close_frames(argv[2]);
        else
#endif
        {
            resize_surfaces();
            exercise();
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SetThreadDesktop(previous);
    CloseDesktop(private_desk);
    return status;
}
