#pragma once
#include <shlobj.h>
#include <memory>
#include <string>

namespace cliprope::win {
inline bool system_snip_source(const std::string& source) {
    return _stricmp(source.c_str(), "SnippingTool.exe") == 0;
}

inline std::wstring native_screenshot_folder() {
    PWSTR raw{};
    const auto result = SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
    return SUCCEEDED(result) && folder ? std::wstring(folder.get()) : std::wstring{};
}
}
