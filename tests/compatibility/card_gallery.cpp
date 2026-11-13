#include "platform_win/workspace.hpp"
#include "presentation/hanging_layout.hpp"
#include "presentation/swing.hpp"
#include "rendering/workspace_paint.hpp"
#include "workspace_fixture_frame.hpp"
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace cliprope;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}

// A synthetic landscape, so the image template has real pixels to fit.
std::shared_ptr<domain::ImagePreview> landscape(unsigned width = 160, unsigned height = 110) {
    auto image = std::make_shared<domain::ImagePreview>();
    image->width = width;
    image->height = height;
    image->pbgra.resize(std::size_t(image->width) * image->height * 4);
    for (unsigned y = 0; y < image->height; ++y)
        for (unsigned x = 0; x < image->width; ++x) {
            auto *p = &image->pbgra[(std::size_t(y) * image->width + x) * 4];
            const float u = float(x) / width, v = float(y) / height;
            const bool hill = v > .64f + .13f * std::sin(u * 7.3f);
            p[0] = static_cast<unsigned char>(hill ? 70 : 235 - 110 * v);
            p[1] = static_cast<unsigned char>(hill ? 140 : 190 - 37 * v);
            p[2] = static_cast<unsigned char>(hill ? 60 : 120 + 55 * v);
            p[3] = 255;
        }
    return image;
}

std::vector<application::CardSummary> samples() {
    using application::CardKind;
    std::vector<application::CardSummary> cards(8);
    const char *ids[] = {"text", "link", "image", "file", "folder", "list", "rich", "missing"};
    for (std::size_t i = 0; i < cards.size(); ++i)
        cards[i].entry.id = cards[i].entry.snapshot_id = ids[i];
    cards[0].text = L"Quarterly review notes\r\nCheck the figures before Friday and send the summary to the "
                    L"team. The rest of this sample keeps going so that its last line fades out.";
    cards[1].kind = CardKind::link;
    cards[1].text = L"https://example.com/reports/2026/q3";
    cards[2].kind = CardKind::image;
    cards[2].image_primary = true;
    cards[2].thumbnail = landscape();
    cards[2].source = {160, 110};
    cards[3].kind = CardKind::files;
    cards[3].text = L"report.pdf";
    cards[3].location = L"D:\\Archive\\Projects\\Atlas\\2026";
    cards[3].items = 1;
    cards[4].kind = CardKind::folder;
    cards[4].text = L"Atlas";
    cards[4].location = L"D:\\Archive\\Projects";
    cards[4].items = 1;
    cards[5].kind = CardKind::files;
    cards[5].text = L"figure-01.png";
    cards[5].location = L"D:\\Archive\\Figures";
    cards[5].items = 12;
    cards[6].kind = CardKind::rich;
    cards[6].status = application::CardStatus::partial;
    cards[7].kind = CardKind::unavailable;
    cards[7].status = application::CardStatus::damaged;
    return cards;
}

struct Pixels {
    LONG width{}, height{};
    std::vector<DWORD> data;
    DWORD at(LONG x, LONG y) const { return data[std::size_t(y) * width + x] & 0xffffff; }
};
Pixels print(HWND hwnd) {
    RECT size{};
    GetClientRect(hwnd, &size);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), size.right, -size.bottom, 1, 32};
    HDC dc = CreateCompatibleDC(nullptr);
    void *bits{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    require(dc && bitmap && bits, "Gallery surface failed");
    auto previous = SelectObject(dc, bitmap);
    SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    Pixels result{size.right, size.bottom};
    const auto *source = static_cast<const DWORD *>(bits);
    result.data.assign(source, source + std::size_t(size.right) * size.bottom);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return result;
}
int distance(DWORD a, DWORD b) {
    int worst = 0;
    for (int shift : {0, 8, 16})
        worst = std::max(worst, std::abs(int((a >> shift) & 255) - int((b >> shift) & 255)));
    return worst;
}
int count(const Pixels &p, RECT r, DWORD rgb, int tolerance) {
    int n = 0;
    for (LONG y = std::max(0L, r.top); y < std::min(p.height, r.bottom); ++y)
        for (LONG x = std::max(0L, r.left); x < std::min(p.width, r.right); ++x)
            n += distance(p.at(x, y), rgb) <= tolerance;
    return n;
}
int changed(const Pixels &a, const Pixels &b, RECT r, int tolerance) {
    int n = 0;
    for (LONG y = std::max(0L, r.top); y < std::min(a.height, r.bottom); ++y)
        for (LONG x = std::max(0L, r.left); x < std::min(a.width, r.right); ++x)
            n += distance(a.at(x, y), b.at(x, y)) > tolerance;
    return n;
}
int luminance(DWORD rgb) { return int(rgb & 255) + int((rgb >> 8) & 255) + int((rgb >> 16) & 255); }

void write_bmp(const std::filesystem::path &path, LONG width, LONG height, const void *bits) {
    BITMAPINFOHEADER header{sizeof(header), width, -height, 1, 32};
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + DWORD(4 * width * height);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(&file), sizeof(file));
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    out.write(static_cast<const char *>(bits), file.bfSize - file.bfOffBits);
    require(bool(out), "Could not record gallery frame");
}

void text_size_range() {
    constexpr int w = 480, h = 400;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32};
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    require(dc && bitmap && bits, "Missing style test surface");
    const auto old = SelectObject(dc, bitmap);
    auto paint = rendering::WorkspacePaint::acquire();
    auto card = samples().front();
    card.text = L"H";
    card.style.text_color = 0xff00ff;
    auto rows = [&](float size, int from, int to) {
        card.style.font_size = size;
        require(paint->draw_floating(dc, {0,0,w,h}, 96, card, rendering::FloatingControl::none),
                "Failed to draw styled text");
        GdiFlush();
        int height = 0;
        for (int y = from; y < to; ++y) {
            const auto row = static_cast<const DWORD*>(bits) + y * w;
            if (std::any_of(row, row + w, [](DWORD pixel) {
                return (pixel & 0xffffff) == 0xff00ff;
            })) ++height;
        }
        return height;
    };
    const int small = rows(72, 45, h);
    require(small > 0 && rows(96, 45, h) > small * 1.2, "96-DIP glyphs did not grow past 72 DIPs");
    card.kind = application::CardKind::link;
    card.text = L"https://example.com/";
    require(rows(32, 44, 92) > 0 && rows(32, 96, h) > 0, "URL title or body ignored its text color");
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// Three Floating sheets side by side, as the Floating renderer draws them.
void floating_strip(const std::vector<application::CardSummary> &cards, const std::filesystem::path &path) {
    constexpr LONG w = 240, h = 160, gap = 16;
    const std::size_t picks[] = {0, 2, 3};
    const LONG total = LONG(std::size(picks)) * (w + gap) + gap;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), total, -(h + 2 * gap), 1, 32};
    HDC dc = CreateCompatibleDC(nullptr);
    void *bits{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    require(dc && bitmap && bits, "Floating gallery surface failed");
    auto previous = SelectObject(dc, bitmap);
    RECT all{0, 0, total, h + 2 * gap};
    SetDCBrushColor(dc, RGB(53, 57, 56));
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const auto paint = rendering::WorkspacePaint::acquire();
    for (std::size_t i = 0; i < std::size(picks); ++i) {
        HDC sheet = CreateCompatibleDC(dc);
        HBITMAP image = CreateCompatibleBitmap(dc, w, h);
        auto old = SelectObject(sheet, image);
        require(paint->draw_floating(sheet, {0, 0, w, h}, 96, cards[picks[i]],
                                     i == 0 ? rendering::FloatingControl::restore : rendering::FloatingControl::none),
                "Floating sheet failed to draw");
        BitBlt(dc, gap + LONG(i) * (w + gap), gap, w, h, sheet, 0, 0, SRCCOPY);
        SelectObject(sheet, old);
        DeleteObject(image);
        DeleteDC(sheet);
    }
    GdiFlush();
    write_bmp(path, total, h + 2 * gap, bits);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

template <class Reads> void serve(Reads &reads, const std::vector<application::CardSummary> &cards) {
    reads.count = [&](auto) { return cards.size(); };
    reads.page = [&](auto, std::size_t offset, std::size_t n) {
        return std::vector<application::CardSummary>(cards.begin() + std::min(offset, cards.size()),
                                                     cards.begin() + std::min(offset + n, cards.size()));
    };
    reads.position = [&](auto, std::string_view id) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < cards.size(); ++i)
            if (cards[i].entry.id == id)
                return i;
        return {};
    };
    reads.load_anchor = [](auto) { return application::Anchor{}; };
    reads.save_anchor = [](auto, auto &) {};
}

void resize_gallery(const std::filesystem::path& folder) {
    auto source = samples();
    auto portrait = source[2];
    portrait.entry.id = "portrait";
    portrait.source = {90, 300};
    portrait.thumbnail = landscape(90, 300);
    std::vector<application::CardSummary> cards{source[1], source[0], portrait, source[2]};
    cards[0].scale = {.75f, .75f};
    cards[2].scale = {2, 2};
    presentation::ViewportReads reads;
    serve(reads, cards);
    const domain::Rope rope{"main", "Sizing", 100, true};
    auto paint = rendering::WorkspacePaint::acquire();
    std::filesystem::create_directories(folder);
    for (const auto dpi : {96, 144, 192}) {
        presentation::Viewport v(reads);
        v.open("main", 1110);
        // Leave room to show both ends of the smallest hanger at the final frame.
        const float hook = v.left(1) + v.geometry(1).width;
        rendering::PaintedRow row{&rope, &v, 0};
        const RECT area{0, 0, MulDiv(1110, dpi, 96), MulDiv(448, dpi, 96)};
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), area.right, -area.bottom, 1, 32};
        void* bits{};
        auto image = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        require(dc && image && bits, "Resize reference DC unavailable");
        const auto old = SelectObject(dc, image);
        const auto start = std::chrono::steady_clock::now();
        auto warm = start;
        DWORD gdi = 0;
        std::uint64_t targets = 0;
        constexpr int steps = 24;
        for (int step = 0; step <= steps; ++step) {
            const float scale = 1.f + float(step) / steps;
            v.resize_card(1, {scale, scale}, hook);
            SetDCBrushColor(dc, RGB(53, 57, 56));
            FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            require(rendering::print_window(dc, area, [&](HDC dest) {
                return paint->draw(dest, area, dpi, {&row, 1}, {});
            }), "Resize sequence rendering failed");
            GdiFlush();
            if (!step) {
                warm = std::chrono::steady_clock::now();
                gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                targets = rendering::WorkspacePaint::target_creations();
            }
        }
        const auto ms = [](auto elapsed) { return std::chrono::duration<double, std::milli>(elapsed).count(); };
        std::cout << "Resize " << dpi << " DPI, first=" << ms(warm-start) << " ms, mean frame="
                  << ms(std::chrono::steady_clock::now()-warm)/steps << " ms, GDI delta="
                  << long(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS))-long(gdi)
                  << ", new targets=" << rendering::WorkspacePaint::target_creations()-targets << '\n';
        write_bmp(folder / ("hanging-" + std::to_string(dpi) + ".bmp"), area.right, area.bottom, bits);
        auto floating = cards[1];
        floating.scale = {1.5f, 1.f};
        const RECT window{0, 0, MulDiv(360, dpi, 96), MulDiv(160, dpi, 96)};
        require(paint->draw_floating(dc, window, dpi, floating, rendering::FloatingControl::body), "Floating resize frame failed");
        GdiFlush();
        // Copy the window-sized subrectangle out of the larger DIB stride.
        std::vector<DWORD> crop(size_t(window.right) * window.bottom);
        for (LONG y = 0; y < window.bottom; ++y)
            std::copy_n(static_cast<DWORD*>(bits) + y * area.right, window.right, crop.data() + y * window.right);
        write_bmp(folder / ("floating-" + std::to_string(dpi) + ".bmp"), window.right, window.bottom, crop.data());
        SelectObject(dc, old);
        DeleteObject(image);
        DeleteDC(dc);
    }
}

// WM_PRINTCLIENT prints the still scene, so the hover frame calls the renderer directly.
void adaptive_gallery(const std::filesystem::path& frame) {
    using application::CardSummary;
    std::vector<CardSummary> cards(3);
    const domain::ImageMetadata sizes[]{{1920,1080}, {1024,1024}, {1080,1920}};
    for (int i = 0; i < 3; ++i) {
        auto& card = cards[i];
        card.entry.id = card.entry.snapshot_id = std::to_string(i);
        card.kind = i == 2 ? application::CardKind::files : application::CardKind::image;
        if (i == 2) card.file_preview = CardSummary::FilePreviewKind::picture;
        card.source = sizes[i];
        auto thumb = landscape(card.source.width / 8, card.source.height / 8);
        if (i == 2) {
            for (unsigned y = 16; y < 48; ++y)
                for (unsigned x = 16; x < 48; ++x)
                    std::fill_n(&thumb->pbgra[(y * thumb->width + x) * 4], 4, 0);
        }
        card.thumbnail = std::move(thumb);
    }
    win::WorkspaceReads reads;
    serve(reads, cards);
    win::Workspace workspace(std::move(reads), {});
    require(workspace.open(), "Adaptive workspace did not open");
    workspace.show("main");
    const auto hwnd = workspace.window();
    const auto dpi = GetDpiForWindow(hwnd);
    const auto px = [&](float dip) { return MulDiv(int(std::lround(dip * 256)), dpi, 96 * 256); };
    RECT previous{};
    for (std::size_t i = 0; i < cards.size(); ++i) {
        const auto g = presentation::ImageCardLayout::measure(cards[i]);
        const auto rect = workspace.card_screen_rect(i);
        require(std::abs(rect.right - rect.left - px(g.body_width)) <= 1 &&
                std::abs(rect.bottom - rect.top - px(g.height())) <= 1, "Presented image has incorrect bounds");
        if (i) require(rect.left - previous.right >= px(24), "Adjacent images have insufficient clearance");
        previous = rect;
    }
    const auto portrait = workspace.card_screen_rect(2);
    POINT beside{portrait.left - px(12), (portrait.top + portrait.bottom) / 2};
    require(WindowFromPoint(beside) != hwnd, "Space beside portrait still intercepts the pointer");
    if (!frame.empty()) workspace_fixture::frame(hwnd, frame);
    const auto plain = print(hwnd);
    cards[2].image_preferences.checkerboard = true;
    workspace.refresh();
    const auto checked = print(hwnd);
    auto patch = portrait;
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&patch), 2);
    const auto& thumb = *cards[2].thumbnail;
    const auto w = patch.right - patch.left, h = patch.bottom - patch.top;
    patch = {patch.left + MulDiv(w, 20, thumb.width), patch.top + MulDiv(h, 20, thumb.height),
             patch.left + MulDiv(w, 44, thumb.width), patch.top + MulDiv(h, 44, thumb.height)};
    require(changed(plain, checked, patch, 35) > 50, "Checkerboard did not reach transparent source pixels");
    SendMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::density), 0);
    previous = {};
    for (std::size_t i = 0; i < cards.size(); ++i) {
        const auto g = presentation::ImageCardLayout::measure(cards[i], 86, true);
        const auto rect = workspace.card_screen_rect(i);
        require(std::abs(rect.right - rect.left - px(g.body_width)) <= 1 &&
                std::abs(rect.bottom - rect.top - px(g.height())) <= 1,
                "Compact image paint and hit bounds disagree with geometry");
        if (i) require(rect.left - previous.right >= px(12), "Compact image gap is too narrow");
        previous = rect;
    }
    if (!frame.empty()) workspace_fixture::frame(hwnd, frame.parent_path() / (frame.stem().wstring() + L"-compact.bmp"));
}
void hover(std::vector<application::CardSummary> cards, const std::filesystem::path &path) {
    using presentation::Hanging;
    constexpr LONG width = 1000, height = 440;
    presentation::ViewportReads reads;
    serve(reads, cards);
    presentation::Viewport view(reads);
    view.open("main", float(width));
    const domain::Rope rope{"main", "Main", 50, true, 0};
    rendering::PaintedRow row{&rope, &view, 0};
    const auto paint = rendering::WorkspacePaint::acquire();
    const RECT bounds{0, 0, width, height};
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32};
    HDC dc = CreateCompatibleDC(nullptr);
    void *bits{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    require(dc && bitmap && bits, "Hover surface failed");
    auto previous = SelectObject(dc, bitmap);
    const auto render = [&](std::string_view hovered, const presentation::Swing *swing = nullptr,
                            const application::SearchHit* hit = nullptr, std::wstring_view term = {}, unsigned match = 0xe5a85e) {
        SetDCBrushColor(dc, RGB(53, 57, 56));
        FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        rendering::Feedback state;
        state.hovered = hovered;
        state.find_hit = hit;
        state.find_term = term;
        state.match_color = match;
        state.swing = swing;
        require(rendering::print_window(dc, bounds,
                                        [&](HDC dest) { return paint->draw(dest, bounds, 96, {&row, 1}, state); }),
                "Hover frame failed to draw");
        GdiFlush();
        const auto *source = static_cast<const DWORD *>(bits);
        return Pixels{width, height, std::vector<DWORD>(source, source + std::size_t(width) * height)};
    };
    cards[2].source = {1920,1080};
    view.refresh(float(width));
    const auto still = render({});
    application::SearchHit deep{"text","text","main",0,application::SearchField::body,
        {3800,2},L"near 😀磁性\r\n磁性",3793};
    const auto marked=render({},nullptr,&deep,L"磁性");
    require(count(marked,bounds,0xe5a85e,3)>12 && count(marked,bounds,0xf3de9a,3)>12,
            "Deep context did not draw both the active and ordinary occurrence");
    auto custom = render({},nullptr,&deep,L"磁性",0x6dacdf);
    require(count(custom,bounds,0x6dacdf,3)>12 && count(custom,bounds,0xf3de9a,3)==count(marked,bounds,0xf3de9a,3),
            "Changing the selected match color affected the ordinary occurrence");
    auto next = deep;
    next.range = {3804, 2};
    const auto advanced = render({}, nullptr, &next, L"磁性", 0x6dacdf);
    int released = 0, selected = 0;
    for (size_t i = 0; i < custom.data.size(); ++i) {
        const auto before = custom.data[i] & 0xffffff, after = advanced.data[i] & 0xffffff;
        released += before == 0x6dacdf && after == 0xf3de9a;
        selected += before == 0xf3de9a && after == 0x6dacdf;
    }
    require(released > 12 && selected > 12, "Next occurrence did not exchange the two highlight backgrounds");
    if (!path.empty()) write_bmp(path.parent_path()/"find-deep.bmp",width,height,marked.data.data());
    application::SearchHit filename{"image","image","main",0,application::SearchField::path,
        {5,6},L"demo/purple.png",0};
    const auto image_match=render({},nullptr,&filename,L"purple");
    require(count(image_match,bounds,0xe5a85e,3)>12,"Missing adaptive image filename highlight");
    require(changed(still,render({}),bounds,0)==0,"Closing find failed to restore baseline card pixels");
    const auto lifted = render("image");
    const presentation::RopeCurve curve{float(width)};
    const auto paper = [&](std::size_t i, float &hook, float &rope_y) {
        const auto g = view.geometry(i);
        hook = view.left(i) + g.width / 2;
        rope_y = curve.contact(hook);
        return RECT{LONG(hook - g.body_width / 2), LONG(rope_y + g.top - (g.adaptive ? 0 : Hanging::paper_tuck)),
                    LONG(hook + g.body_width / 2), LONG(rope_y + g.bottom)};
    };
    float hook{}, rope_y{};
    auto footer = paper(2, hook, rope_y);
    footer.top = footer.bottom - 20;
    cards[2].hover_resolution = false;
    view.refresh(float(width));
    const auto without_dimensions = render("image");
    require(changed(lifted, without_dimensions, footer, 16) > 70 &&
            changed(still, render({}), bounds, 0) == 0, "Resolution setting did not affect the hovered image footer");
    cards[2].hover_resolution = true;
    view.refresh(float(width));
    const LONG cx = LONG(hook + view.geometry(2).width / 2 - 38),
               cy = LONG(rope_y + view.geometry(2).bar_bottom - 12);
    require(changed(still, lifted, {cx - 7, cy - 7, cx + 7, cy + 7}, 40) > 12, "Hover shows no close control");
    const LONG below = LONG(rope_y + view.geometry(2).bottom + 4);
    require(luminance(lifted.at(LONG(hook), below)) + 6 < luminance(still.at(LONG(hook), below)),
            "Hover does not deepen the shadow");
    for (std::size_t i : {1, 3})
        require(!changed(still, lifted, paper(i, hook, rope_y), 0), "Hover changed a neighboring card");
    if (!path.empty())
        write_bmp(path, width, height, lifted.data.data());
    // The first animation frame has zero rotation, so it can be compared with
    // a fresh vector drawing. Replace a thumbnail while its card is swinging.
    presentation::Swing swing;
    swing.kick("image", 20);
    const auto image_area = paper(2, hook, rope_y);
    const auto cached = render({}, &swing);
    require(changed(still, cached, image_area, 12) < 20, "Moving face lost its paper or preview");
    cards[2].thumbnail.reset();
    view.refresh(float(width));
    const auto without_preview = render({}, &swing);
    require(changed(cached, without_preview, image_area, 40) > 1000, "Moving face retained a removed preview");
    require(changed(render({}), without_preview, image_area, 12) < 20, "Refreshed face differs from its contents");
    // Carry card 1 into slot 3. Compare the neighboring hangers, including
    // their numbers, against the same order after a real viewport refresh.
    row.preview = {0, 2};
    const auto preview = render({});
    require(preview.at(LONG(view.projected_left(2, row.preview) + 32), 150) == 0x353938,
            "The insertion gap still contains a second sheet");
    row.preview = {};
    std::rotate(cards.begin(), cards.begin() + 1, cards.begin() + 3);
    view.refresh(float(width));
    const auto committed = render({});
    for (std::size_t slot : {0, 1, 3}) {
        auto r = paper(slot, hook, rope_y);
        r.top = LONG(rope_y + Hanging::bar_top);
        require(!changed(preview, committed, r, 0), "Previewed hanger number or paper differs from the drop");
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void observe_opening_swing(const win::Workspace &workspace) {
    const auto hwnd = workspace.window();
    const auto rect = workspace.slots().front().second;
    const int radius = MulDiv(24, GetDpiForWindow(hwnd), 96);
    const auto edge = [&] {
        const LONG y = rect.bottom - radius;
        for (LONG x = rect.left - radius; x < rect.left + radius; ++x)
            if (WindowFromPoint({x, y}) == hwnd)
                return x;
        throw std::runtime_error("Could not locate the presented swinging edge");
    };
    const auto rest = edge();
    auto left = rest, right = rest;
    auto frames = workspace.painted_frames();
    const auto start = GetTickCount64();
    auto changed_at = start;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        if (frames != workspace.painted_frames()) {
            const auto x = edge();
            left = std::min(left, x);
            right = std::max(right, x);
            frames = workspace.painted_frames();
            changed_at = GetTickCount64();
        }
        if (left < rest && right > rest && GetTickCount64() - changed_at > 200)
            break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 12, QS_ALLINPUT);
    } while (GetTickCount64() - start < 4000);
    require(left < rest && right > rest && edge() == rest && GetTickCount64() - changed_at > 200,
            "Presented paper did not swing both ways and come to rest");
    std::cout << "Opening paper edge travelled " << left - rest << "..+" << right - rest
              << " pixels and stopped at its original position\n";
}

void exercise(const std::filesystem::path &frame, const std::filesystem::path &floating,
              const std::filesystem::path &hovered) {
    auto cards = samples();
    int errors = 0;
    win::WorkspaceReads reads;
    serve(reads, cards);
    reads.rope_count = [] { return std::size_t{1}; };
    reads.ropes = [](std::size_t, std::size_t) { return std::vector<domain::Rope>{{"main", "Main", 50, true, 0}}; };
    reads.rope_position = [](auto) { return std::optional<std::size_t>{0}; };
    win::WorkspaceActions actions;
    actions.error = [&] { ++errors; };
    const win::Workspace *self = nullptr;
    std::uint64_t presented_at_report = 0;
    actions.layout_changed = [&] { presented_at_report = self ? self->painted_frames() : 0; };
    win::Workspace workspace(std::move(reads), std::move(actions));
    self = &workspace;
    require(workspace.open(), "Gallery workspace failed to open");
    workspace.show("main");
    const auto hwnd = workspace.window();
    require(workspace.painted_frames() > 0 && !errors, "Gallery scene was not presented");
    const auto pixels = print(hwnd);
    const auto dpi = GetDpiForWindow(hwnd);
    const auto px = [&](double dip) { return LONG(std::lround(dip * dpi / 96)); };
    std::size_t checked = 0;
    for (auto [id, slot] : workspace.slots()) {
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&slot), 2);
        if (slot.right - slot.left < px(172) - 1)
            continue; // Clipped by the window edge.
        if (id == "image") {
            require(std::abs((slot.right - slot.left) - px(192)) <= 1 &&
                    std::abs((slot.bottom - slot.top) - px(132)) <= 1, "Source dimensions did not size the image");
            const RECT interior{slot.left + px(2), slot.top + px(4), slot.right - px(2), slot.bottom - px(2)};
            require(count(pixels, interior, 0xfbf7ef, 12) == 0, "Adaptive image contains a paper mat");
            ++checked;
            continue;
        }
        // The bar spans the 24 DIPs above the visible paper; its label sits on the left.
        const RECT label{slot.left + px(4), slot.top - px(16), slot.left + px(60), slot.top - px(2)};
        const RECT icon{slot.left + px(153), slot.top - px(15), slot.left + px(168), slot.top - px(1)};
        require(count(pixels, label, 0x5e5144, 40) > 8, "Hanger lacks its engraved number");
        require(count(pixels, icon, 0x6c5a4c, 40) > 4, "Hanger lacks its type icon");
        // The paper keeps no header: the band above the content is plain paper.
        const RECT band{slot.left + px(12), slot.top + px(8), slot.right - px(12), slot.top + px(11)};
        const auto paper = pixels.at(slot.left + px(3), (slot.top + slot.bottom) / 2);
        require(count(pixels, band, paper, 24) == (band.right - band.left) * (band.bottom - band.top),
                "Paper shows a header above its content");
        ++checked;
    }
    require(checked >= 5, "Too few unclipped gallery cards");
    if (!frame.empty())
        workspace_fixture::frame(hwnd, frame);
    if (!floating.empty())
        floating_strip(cards, floating);
    observe_opening_swing(workspace);
    // Windows hit-tests the presented alpha, so a reported layout must already be on screen.
    // Some stations never send WM_PAINT to a layered window, so no message pump runs here.
    const auto before = workspace.painted_frames();
    workspace.set_overview(true);
    require(presented_at_report > before, "A layout was reported before its frame was presented");
    workspace.hide();
    require(!errors, "A gallery callback failed");
    hover(cards, hovered);
    std::cout << "Card gallery: " << checked << " hangers carry number and icon; papers carry no header; "
              << "hover adds the close control and a deeper shadow to one card; a new layout is presented "
              << "before it is reported\n";
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool frames = argc == 5 && std::wstring_view(argv[1]) == L"--frame";
    const bool adaptive = argc == 3 && std::wstring_view(argv[1]) == L"--adaptive-frame";
    const bool resize = argc == 3 && std::wstring_view(argv[1]) == L"--resize";
    if (argc != 1 && !frames && !adaptive && !resize)
        return 2;
    const auto name = L"ClipRope.Gallery." + std::to_wstring(GetCurrentProcessId());
    const auto previous = GetThreadDesktop(GetCurrentThreadId());
    HDESK desk = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desk || !SetThreadDesktop(desk)) {
        std::cerr << "Private desktop unavailable\n";
        return 1;
    }
    int status = 0;
    try {
        if (resize) resize_gallery(argv[2]);
        adaptive_gallery(adaptive ? argv[2] : L"");
        text_size_range();
        exercise(frames ? argv[2] : L"", frames ? argv[3] : L"", frames ? argv[4] : L"");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SetThreadDesktop(previous);
    CloseDesktop(desk);
    return status;
}
