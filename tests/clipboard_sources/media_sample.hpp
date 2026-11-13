#pragma once
#include "sample.hpp"
#include <algorithm>
#include <cstring>

namespace media_sample {
using cliprope::Representation;
inline Representation png() {
    return {RegisterClipboardFormatW(L"PNG"),
            {137, 80,  78,  71,  13,  10, 26,  10,  0,   0,   0,   13,  73,  72, 68,  82, 0,  0,  0,
             2,   0,   0,   0,   2,   8,  6,   0,   0,   0,   114, 182, 13,  36, 0,   0,  0,  18, 73,
             68,  65,  84,  120, 156, 99, 248, 207, 192, 240, 31,  12,  129, 52, 24,  0,  0,  73, 200,
             9,   247, 249, 171, 182, 13, 0,   0,   0,   0,   73,  69,  78,  68, 174, 66, 96, 130}};
}
inline Representation dib(bool v5) {
    BITMAPV5HEADER h{};
    h.bV5Size = v5 ? sizeof h : sizeof(BITMAPINFOHEADER);
    h.bV5Width = 2;
    h.bV5Height = 2;
    h.bV5Planes = 1;
    h.bV5BitCount = 24;
    h.bV5SizeImage = 16;
    Representation r{v5 ? UINT(CF_DIBV5) : UINT(CF_DIB), std::vector<unsigned char>(h.bV5Size)};
    std::memcpy(r.bytes.data(), &h, h.bV5Size);
    // Two bottom-up BGR rows at an eight-byte stride: blue/white below red/green.
    r.bytes.insert(r.bytes.end(), {255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0});
    return r;
}
inline Representation stream_png(){
    auto result=png();result.tymed=TYMED_ISTREAM;return result;
}
inline std::vector<Representation> rich() {
    auto formats = sample::formats(false);
    constexpr char rtf[] = "{\\rtf1\\ansi\\b Sample \\u937?\\b0}";
    formats.push_back({RegisterClipboardFormatW(L"Rich Text Format"), {rtf, rtf + sizeof rtf}});
    formats.push_back(png());
    formats.push_back(dib(false));
    formats.push_back(dib(true));
    return formats;
}
inline std::vector<Representation> formats(std::wstring_view mode) {
    if (mode == L"image")
        return {stream_png()};
    if (mode == L"partial" || mode == L"bad") {
        auto formats = sample::formats(false);
        const auto p =
            std::search(formats[1].bytes.begin(), formats[1].bytes.end(), "EndHTML:", "EndHTML:" + 8);
        if (p != formats[1].bytes.end())
            std::fill_n(p + 8, 10, static_cast<unsigned char>('9'));
        auto bitmap = dib(false);
        std::fill_n(bitmap.bytes.begin() + 4, 4, static_cast<unsigned char>(0x7f));
        formats.push_back(std::move(bitmap));
        auto truncated = dib(true);
        truncated.bytes.resize(80);
        formats.push_back(std::move(truncated));
        if (mode == L"bad")
            formats.erase(formats.begin());
        return formats;
    }
    auto formats = rich();
    // A producer can use an oversized HGLOBAL for a packed DIB. Padding
    // belongs to the source allocation, not the saved representation.
    for (auto& format : formats)
        if (format.format == CF_DIB || format.format == CF_DIBV5)
            format.bytes.insert(format.bytes.end(), 64, 0xa5);
    formats.push_back({RegisterClipboardFormatW(L"Unverified Office-private fixture"), {1, 2, 3}});
    return formats;
}
} // namespace media_sample
