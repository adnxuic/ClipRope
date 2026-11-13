#pragma once

#include "platform_win/clipboard.hpp"
#include <cstdio>

namespace sample {

inline std::wstring text(bool second) { return second ? L"Second synthetic copy" : L"Sample \u03A9"; }

inline std::vector<cliprope::Representation> formats(bool second) {
    const auto value = text(second);
    const auto* data = reinterpret_cast<const unsigned char*>(value.c_str());
    const std::string fragment = second ? "<b>Second synthetic copy</b>" : "<b>Sample \xCE\xA9</b>";
    const std::string prefix = "<html><body><!--StartFragment-->";
    const std::string suffix = "<!--EndFragment--></body></html>";
    const char* pattern = "Version:1.0\r\nStartHTML:%010u\r\nEndHTML:%010u\r\nStartFragment:%010u\r\nEndFragment:%010u\r\n";
    char header[160];
    const auto start = static_cast<unsigned>(std::snprintf(header, sizeof(header), pattern, 0u, 0u, 0u, 0u));
    const auto begin = start + static_cast<unsigned>(prefix.size());
    const auto end = begin + static_cast<unsigned>(fragment.size());
    std::snprintf(header, sizeof(header), pattern, start, end + static_cast<unsigned>(suffix.size()), begin, end);
    auto html = std::string(header) + prefix + fragment + suffix;
    html.push_back('\0');
    return {{CF_UNICODETEXT, {data, data + (value.size() + 1) * sizeof(wchar_t)}},
            {RegisterClipboardFormatW(L"HTML Format"), {html.begin(), html.end()}}};
}

} // namespace sample
