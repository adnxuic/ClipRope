#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace workspace_fixture {
// Render only fixture-owned content. This buffer never reads a screen DC.
inline void frame(HWND hwnd, const std::filesystem::path &path, bool require_region = true, bool children = false) {
    RECT r{};
    GetClientRect(hwnd, &r);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right;
    info.bmiHeader.biHeight = -r.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void *bits{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits)
        throw std::runtime_error("Fixture bitmap unavailable");
    auto previous = SelectObject(dc, bitmap);
    SetDCBrushColor(dc, RGB(53, 57, 56));
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    HRGN clip = CreateRectRgn(0, 0, 0, 0);
    if (GetWindowRgn(hwnd, clip) != ERROR) SelectClipRgn(dc, clip);
    else if (require_region) throw std::runtime_error("Could not inspect workspace region");
    if (children) SendMessageW(hwnd, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0);
    SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    if (children) {
        for (auto child=GetWindow(hwnd,GW_CHILD); child; child=GetWindow(child,GW_HWNDNEXT)) {
            RECT rect{};
            GetWindowRect(child,&rect);
            MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&rect),2);
            const int saved=SaveDC(dc);
            IntersectClipRect(dc,rect.left,rect.top,rect.right,rect.bottom);
            SetViewportOrgEx(dc,rect.left,rect.top,nullptr);
            SendMessageW(child,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
            RestoreDC(dc,saved);
        }
    }
    GdiFlush();
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + 4 * r.right * r.bottom;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<char *>(&header), sizeof(header));
    out.write(reinterpret_cast<char *>(&info.bmiHeader), sizeof(info.bmiHeader));
    out.write(static_cast<char *>(bits), header.bfSize - header.bfOffBits);
    if (!out)
        throw std::runtime_error("Could not record workspace frame");
    SelectObject(dc, previous);
    DeleteObject(clip);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

} // namespace workspace_fixture
