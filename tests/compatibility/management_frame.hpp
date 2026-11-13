#pragma once
#include "../integration/history_fixture.hpp"
#include "platform_win/menu_bitmap.hpp"
#include <fstream>
#include <algorithm>

namespace management_fixture {
inline bool record_frames{};
inline void frame(HWND window, const std::filesystem::path& file) {
    if (!record_frames) return;
    RECT r{};
    GetWindowRect(window, &r);
    const int width = r.right - r.left, height = r.bottom - r.top;
    cliprope::win::MenuBitmap capture;
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    history_fixture::require(capture.create(width, height) && PrintWindow(window, capture.dc, PW_RENDERFULLCONTENT),
                             "Fixture-owned window print failed");
    GdiFlush();
    const auto count = width * height;
    const auto first = capture.pixels[0] & 0xffffff;
    history_fixture::require(std::count_if(capture.pixels, capture.pixels + count, [=](auto c) {
        return (c & 0xffffff) != first;
    }) > count / 100, "Composited management print has no visible content");
    const DWORD bytes = 4 * count;
    const BITMAPFILEHEADER header{0x4d42, 54 + bytes, 0, 0, 54};
    const BITMAPINFOHEADER info{sizeof info, width, -height, 1, 32, BI_RGB};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof header);
    out.write(reinterpret_cast<const char*>(&info), sizeof info);
    out.write(reinterpret_cast<const char*>(capture.pixels), bytes);
    history_fixture::require(bool(out), "Failed to save control frame");
}
}
