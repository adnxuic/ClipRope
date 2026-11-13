#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <string_view>

namespace cliprope::win {
inline bool safe_file_basename(std::wstring_view name) {
    if (name.empty() || name.size() >= MAX_PATH || name.back() == L'.' || name.back() == L' ' ||
        name.find_first_of(L"\\/:*?\"<>|") != name.npos ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr))
        return false;
    if (std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 32; })) return false;
    auto stem = std::wstring(name.substr(0, name.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto& c : stem) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" || stem == L"CONOUT$") return false;
    if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        std::wstring_view(L"0123456789\u00B9\u00B2\u00B3").find(stem[3]) != std::wstring_view::npos) return false;
    return true;
}
} // namespace cliprope::win
