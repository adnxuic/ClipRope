#pragma once
#include <cstdint>
#include <windows.h>

namespace cliprope::win {
// A top-down 32-bit DIB with its original DC selection restored on destruction.
class MenuBitmap final {
  public:
    MenuBitmap() = default;
    MenuBitmap(const MenuBitmap&) = delete;
    MenuBitmap& operator=(const MenuBitmap&) = delete;
    ~MenuBitmap() {
        if (previous_) SelectObject(dc, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc) DeleteDC(dc);
    }
    bool create(int width, int height) {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
        dc = CreateCompatibleDC(nullptr);
        void* bits{};
        if (dc) bitmap_ = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap_) previous_ = SelectObject(dc, bitmap_);
        pixels = static_cast<std::uint32_t*>(bits);
        return previous_ && pixels;
    }
    HDC dc{};
    std::uint32_t* pixels{};
  private:
    HBITMAP bitmap_{};
    HGDIOBJ previous_{};
};
} // namespace cliprope::win
