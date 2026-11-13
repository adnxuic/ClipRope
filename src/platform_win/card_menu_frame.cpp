#include "platform_win/card_menu_frame.hpp"
#include "platform_win/menu_bitmap.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <memory>
#include <dwmapi.h>

namespace cliprope::win {
namespace {
constexpr wchar_t frame_class[] = L"ClipRope.CardMenuFrame";
LRESULT CALLBACK frame_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
// Signed distance from the center of a pixel to a continuous rounded rectangle.
float distance(float x, float y, float half_width, float half_height, float radius) {
    const float dx = std::abs(x) - half_width + radius, dy = std::abs(y) - half_height + radius;
    return std::hypot(std::max(0.f, dx), std::max(0.f, dy)) + std::min(0.f, std::max(dx, dy)) - radius;
}
struct FrameKey {
    int width, height;
    UINT dpi;
    COLORREF face, edge;
    bool dark, contrast;
    float corner;
    bool operator==(const FrameKey&) const = default;
};

struct FrameImage {
    FrameKey key;
    MenuBitmap bitmap;
    size_t bytes{};
    explicit FrameImage(FrameKey k) : key(k) {}
    bool render();
};

thread_local std::list<std::unique_ptr<FrameImage>> cache;
thread_local size_t resident_bytes{};
thread_local unsigned rasterizations{};

bool FrameImage::render() {
    const auto [width, height, dpi, face, edge, dark, contrast, corner] = key;
    const int pad = CardMenuFrame::shadow_margin(dpi);
    const int w = width + 2 * pad, h = height + 2 * pad;
    const float scale = dpi / 96.f;
    const float radius = std::min({corner * scale, width * .5f, height * .5f});
    if (!bitmap.create(w, h)) return false;
    bytes = size_t(w) * h * sizeof(std::uint32_t);
    auto rgb = [](COLORREF c) {
        return (unsigned(GetRValue(c)) << 16) | (unsigned(GetGValue(c)) << 8) | unsigned(GetBValue(c));
    };
    const auto opaque = 0xff000000u | rgb(face);
    auto pixel = [&](int x, int y) {
        const float dx = x + .5f - pad - width * .5f, dy = y + .5f - pad - height * .5f;
        if (std::abs(dx) < width * .5f - radius - 2 && std::abs(dy) < height * .5f - radius - 2) {
            return opaque;
        }
        const float d = distance(dx, dy, width * .5f, height * .5f, radius);
        const float coverage = std::clamp(.5f - d, 0.f, 1.f);
        const float inside = std::clamp(.5f - scale - d, 0.f, 1.f);
        const float sd =
            std::max(0.f, distance(dx, dy - 4 * scale, width * .5f, height * .5f, radius)) / scale;
        const float shadow = contrast ? 0.f : (dark ? .32f : .16f) * std::exp(-sd * sd / 72.f);
        const float under = shadow * (1 - coverage), line = coverage - inside;
        auto byte = [](float n) { return unsigned(std::lround(std::clamp(n, 0.f, 255.f))); };
        auto channel = [&](BYTE f, BYTE e, int shade) { return byte(f * inside + e * line + shade * under); };
        return (byte(255 * (coverage + under)) << 24) |
               (channel(GetRValue(face), GetRValue(edge), 51) << 16) |
               (channel(GetGValue(face), GetGValue(edge), 40) << 8) |
               channel(GetBValue(face), GetBValue(edge), 31);
    };
    const int side = std::min(w / 2, pad + int(std::ceil(radius)) + 2);
    const int end_top = pad + int(std::ceil(radius + 4 * scale)) + 2;
    for (int y = 0; y < h; ++y) {
        auto* row = bitmap.pixels + size_t(y) * w;
        if (y > end_top && y < h - end_top) {
            std::copy_n(row - w, w, row);
            continue;
        }
        for (int x = 0; x < side; ++x)
            row[w - 1 - x] = row[x] = pixel(x, y);
        std::fill(row + side, row + w - side, pixel(w / 2, y));
    }
    ++rasterizations;
    return true;
}
} // namespace
unsigned CardMenuFrame::generations() { return rasterizations; }
size_t CardMenuFrame::cache_bytes() { return resident_bytes; }
CardMenuFrame::~CardMenuFrame() { close(); }
void CardMenuFrame::close() {
    if (window_)
        DestroyWindow(window_);
    window_ = nullptr;
}
bool CardMenuFrame::open(HWND owner) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = frame_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = frame_class;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    window_ =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                        frame_class, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, wc.hInstance, nullptr);
    return window_ != nullptr;
}
bool CardMenuFrame::fit(HWND surface, const fluent::Palette &theme, UINT dpi, float corner) {
    RECT bounds{};
    if (!GetWindowRect(surface, &bounds))
        return false;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width < 1 || height < 1)
        return false;
    const float scale = dpi / 96.f;
    const int pad = shadow_margin(dpi), inset = std::max(2, int(std::ceil(1.6f * scale)));
    const float radius = std::min({corner * scale, width * .5f, height * .5f});
    const int w = width + 2 * pad, h = height + 2 * pad;
    FrameKey key{width,      height,         dpi,   theme.panel, theme.contrast ? theme.text : theme.line,
                 theme.dark, theme.contrast, corner};
    auto hit = std::find_if(cache.begin(), cache.end(), [&](const auto& entry) { return entry->key == key; });
    std::unique_ptr<FrameImage> transient;
    FrameImage* image{};
    if (hit != cache.end()) {
        cache.splice(cache.begin(), cache, hit);
        image = cache.front().get();
    } else {
        transient = std::make_unique<FrameImage>(key);
        if (!transient->render()) return false;
        image = transient.get();
        if (image->bytes <= cache_budget) {
            while (resident_bytes + image->bytes > cache_budget) {
                resident_bytes -= cache.back()->bytes;
                cache.pop_back();
            }
            cache.push_front(std::move(transient));
            resident_bytes += image->bytes;
        }
    }
    POINT at{bounds.left - pad, bounds.top - pad}, origin{};
    SIZE size{w, h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    bool ok = UpdateLayeredWindow(window_, nullptr, &at, &size, image->bitmap.dc, &origin, 0, &blend, ULW_ALPHA) != FALSE;
    if (!ok)
        return false;
    const int diameter = int(2 * (radius - inset));
    HRGN region = CreateRoundRectRgn(inset, inset, width - inset + 1, height - inset + 1, diameter, diameter);
    if (!region)
        return false;
    if (!SetWindowRgn(surface, region, TRUE)) {
        DeleteObject(region);
        return false;
    }
    const auto border = DWMWA_COLOR_NONE;
    const auto square = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(surface, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DwmSetWindowAttribute(surface, DWMWA_WINDOW_CORNER_PREFERENCE, &square, sizeof(square));
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    return true;
}
} // namespace cliprope::win
