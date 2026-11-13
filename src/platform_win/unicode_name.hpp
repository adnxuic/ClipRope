#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cliprope::win::unicode {
inline std::wstring wide(std::string_view text) {
    if (text.empty())
        return {};
    const auto length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length)
        throw std::invalid_argument("Invalid Unicode name");
    std::wstring out(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(),
                        length);
    return out;
}
inline std::string utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr,
                                0, nullptr, nullptr);
    if (!n)
        throw std::invalid_argument("Invalid Unicode name");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
} // namespace cliprope::win::unicode
