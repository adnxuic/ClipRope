#include "platform_win/screenshot_hover.hpp"
#include "platform_win/fluent.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace cliprope::win {
std::uint64_t next_preview_owner() {
    static std::atomic<std::uint64_t> value{};
    return ++value;
}
std::optional<ScreenshotPlacement> screenshot_placement(RECT card, RECT work, unsigned sw,
                                                        unsigned sh, UINT dpi) {
    if (!sw || !sh || sw > 8192 || sh > 8192 || std::uint64_t(sw) * sh > 8388608 || !dpi)
        return {};
    const LONG gap = MulDiv(12, dpi, 96), pad = std::max(1, MulDiv(3, dpi, 96));
    const double mw = std::min<double>(MulDiv(960, dpi, 96), (work.right - work.left) * .8);
    const double mh = std::min<double>(MulDiv(720, dpi, 96), (work.bottom - work.top) * .8);
    const double ideal =
        std::min({1., mw / sw, mh / sh, 2048. / sw, 2048. / sh, std::sqrt(2097152. / sw / sh)});
    if (ideal <= 0)
        return {};
    RECT regions[]{{card.right + gap, work.top, work.right, work.bottom},
                   {work.left, work.top, card.left - gap, work.bottom},
                   {work.left, card.bottom + gap, work.right, work.bottom},
                   {work.left, work.top, work.right, card.top - gap}};
    std::optional<ScreenshotPlacement> best;
    std::uint64_t area{};
    for (unsigned i = 0; i < 4; ++i) {
        RECT r{};
        if (!IntersectRect(&r, &regions[i], &work))
            continue;
        const double ratio = std::min({ideal, double(r.right - r.left - pad * 2) / sw,
                                       double(r.bottom - r.top - pad * 2) / sh});
        if (ratio <= 0)
            continue;
        const auto w = static_cast<unsigned>(std::floor(sw * ratio)),
                   h = static_cast<unsigned>(std::floor(sh * ratio));
        if (!w || !h)
            continue;
        LONG x = i == 0   ? r.left
                 : i == 1 ? r.right - LONG(w) - pad * 2
                          : std::clamp(card.left, r.left, r.right - LONG(w) - pad * 2);
        LONG y = i == 2   ? r.top
                 : i == 3 ? r.bottom - LONG(h) - pad * 2
                          : std::clamp(card.top, r.top, r.bottom - LONG(h) - pad * 2);
        ScreenshotPlacement candidate{
            {x, y, x + LONG(w) + pad * 2, y + LONG(h) + pad * 2}, w, h, unsigned(pad)};
        if (std::uint64_t(w) * h > area) {
            best = candidate;
            area = std::uint64_t(w) * h;
        }
        // Preserve right/left/below/above priority when the full preferred size fits.
        if (ratio >= ideal - 1e-9)
            return candidate;
    }
    return best;
}
struct ScreenshotHover::Impl {
    HWND parent{}, window{}, source{}, foreground{};
    rendering::ScreenshotPreviewService service;
    application::ScreenshotPreviewRequest request;
    ScreenshotPlacement placement;
    RECT anchor{};
    std::function<bool()> live;
    application::PreviewDeadline entered{}, requested{};
    std::uint64_t serial{};
    bool enabled = true, dispatched = false, finished = false;
    std::shared_ptr<const domain::ImagePreview> displayed;
    Impl(HWND owner, rendering::ScreenshotPreviewService::Resolve resolve)
        : parent(owner), service(std::move(resolve),
                                 [owner] { PostMessageW(owner, screenshot_preview_ready, 0, 0); }) {
    }
    ~Impl() {
        cancel(true);
        if (window)
            DestroyWindow(window);
    }
    void cancel(bool purge) {
        if (window) {
            KillTimer(window, 1);
            ShowWindow(window, SW_HIDE);
            if (!purge)
                SetTimer(window, 2, 30000, nullptr);
        }
        displayed.reset();
        source = nullptr;
        live = {};
        ++serial;
        service.cancel(purge);
        dispatched = finished = false;
        if (purge && window) {
            auto old = std::exchange(window, nullptr);
            DestroyWindow(old);
        }
    }
    bool create() {
        if (window) {
            KillTimer(window, 2);
            return true;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ClipRope.ScreenshotPreview";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        window = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, parent, nullptr, wc.hInstance, this);
        return window != nullptr;
    }
    bool valid() {
        GUITHREADINFO info{sizeof info};
        if (!source || !IsWindowVisible(source) || GetForegroundWindow() != foreground ||
            GetCapture() || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) || (GetAsyncKeyState(VK_MBUTTON) & 0x8000))
            return false;
        if (GetGUIThreadInfo(GetCurrentThreadId(), &info) &&
            (info.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE)))
            return false;
        return live && live();
    }
    bool present(const domain::ImagePreview &image) {
        const auto pad = placement.padding;
        const unsigned w = image.width + 2 * pad, h = image.height + 2 * pad;
        if (!w || !h || w > 2112 || h > 2112 || std::uint64_t(w) * h > 2300000)
            return false;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(w), -LONG(h), 1, 32, BI_RGB};
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc)
            return false;
        void *buffer{};
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &buffer, nullptr, 0);
        if (!bitmap) {
            DeleteDC(dc);
            return false;
        }
        auto old = SelectObject(dc, bitmap);
        auto *out = static_cast<std::uint32_t *>(buffer);
        const auto palette = fluent::system_palette();
        const auto bg = palette.panel, border = palette.line;
        auto color = [](COLORREF rgb) {
            return 0xff000000u | (GetRValue(rgb) << 16) | (GetGValue(rgb) << 8) | GetBValue(rgb);
        };
        std::fill_n(out, size_t(w) * h, color(border));
        for (unsigned y = 0; y < image.height; ++y)
            for (unsigned x = 0; x < image.width; ++x) {
                const auto *p = image.pbgra.data() + (size_t(y) * image.width + x) * 4;
                const unsigned a = 255 - p[3];
                const auto b = std::min(255u, unsigned(p[0]) + (GetBValue(bg) * a + 127) / 255);
                const auto g = std::min(255u, unsigned(p[1]) + (GetGValue(bg) * a + 127) / 255);
                const auto r = std::min(255u, unsigned(p[2]) + (GetRValue(bg) * a + 127) / 255);
                out[size_t(y + pad) * w + x + pad] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        POINT origin{placement.window.left, placement.window.top}, zero{};
        SIZE size{LONG(w), LONG(h)};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const bool ok = UpdateLayeredWindow(window, nullptr, &origin, &size, dc, &zero, 0, &blend,
                                            ULW_ALPHA) != FALSE;
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (ok)
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return ok;
    }
    void tick() {
        if (!valid()) {
            cancel(false);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!dispatched && now - entered >= std::chrono::milliseconds(500)) {
            dispatched = true;
            requested = now;
            service.request(request);
        }
        if (dispatched && !finished) {
            if (auto result = service.take()) {
                finished = true;
                if (result->request.serial == request.serial &&
                    result->request.owner_generation == request.owner_generation &&
                    result->request.entry.id == request.entry.id &&
                    result->request.entry.snapshot_id == request.entry.snapshot_id &&
                    result->request.entry.revision == request.entry.revision && result->image &&
                    valid() && present(*result->image))
                    displayed = std::move(result->image);
            } else if (now - requested >= std::chrono::seconds(2)) {
                service.cancel();
                finished = true;
            }
        }
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (m == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (m == WM_NCHITTEST)
            return HTTRANSPARENT;
        if (self && m == WM_TIMER && w == 2) {
            self->window = nullptr;
            DestroyWindow(h);
            return 0;
        }
        if (self && m == WM_TIMER && w == 1) {
            try {
                self->tick();
            } catch (...) {
                self->cancel(false);
            }
            return 0;
        }
        if (self && (m == WM_DISPLAYCHANGE || m == WM_SETTINGCHANGE || m == WM_THEMECHANGED))
            self->cancel(false);
        return DefWindowProcW(h, m, w, l);
    }
};
ScreenshotHover::ScreenshotHover(HWND owner, rendering::ScreenshotPreviewService::Resolve resolve)
    : impl_(std::make_unique<Impl>(owner, std::move(resolve))) {}
ScreenshotHover::~ScreenshotHover() = default;
bool ScreenshotHover::offer(HWND owner, std::uint64_t generation,
                            const application::CardSummary &card, RECT anchor,
                            std::function<bool()> live) {
    auto &p = *impl_;
    if (!p.enabled || !application::screenshot_preview_eligible(card)) {
        leave(owner);
        return false;
    }
    const bool same = p.source == owner && p.request.owner_generation == generation &&
                      p.request.entry.id == card.entry.id &&
                      p.request.entry.snapshot_id == card.entry.snapshot_id &&
                      p.request.entry.revision == card.entry.revision;
    if (same && EqualRect(&p.anchor, &anchor))
        return true;
    const auto entered = same ? p.entered : std::chrono::steady_clock::now();
    MONITORINFO monitor{sizeof monitor};
    if (!GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor)) {
        p.cancel(false);
        return true;
    }
    const auto layout = screenshot_placement(anchor, monitor.rcWork, card.source.width,
                                             card.source.height, GetDpiForWindow(owner));
    if (layout && same && p.request.width == layout->width && p.request.height == layout->height) {
        p.anchor = anchor;
        p.placement = *layout;
        if (p.displayed)
            SetWindowPos(p.window, nullptr, layout->window.left, layout->window.top, 0, 0,
                         SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
        return true;
    }
    p.cancel(false);
    if (!layout || !p.create())
        return true;
    p.source = owner;
    p.anchor = anchor;
    p.foreground = GetForegroundWindow();
    p.placement = *layout;
    p.live = std::move(live);
    p.request = {card.entry, generation, ++p.serial, layout->width, layout->height};
    p.entered = entered;
    SetTimer(p.window, 1, 10, nullptr);
    return true;
}
void ScreenshotHover::leave(HWND source) {
    if (impl_->source == source)
        impl_->cancel(false);
}
void ScreenshotHover::cancel(bool purge) { impl_->cancel(purge); }
void ScreenshotHover::ready() {
    if (impl_->source) {
        try {
            impl_->tick();
        } catch (...) {
            impl_->cancel(false);
        }
    }
}
void ScreenshotHover::enabled(bool value) {
    impl_->enabled = value;
    if (!value)
        impl_->cancel(true);
}
rendering::ScreenshotPreviewService::Counters ScreenshotHover::counters() const {
    return impl_->service.counters();
}
} // namespace cliprope::win
