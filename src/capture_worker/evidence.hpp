#pragma once
#include <windows.h>
#include <string_view>

namespace cliprope {
enum class NativeGuard : DWORD {
    none, privacy_uncertain, no_owner, open_failed, changed, enum_failed, first_not_unicode,
    mixed, too_many, empty, invalid_unicode, recovered
};
struct OomEvidence {
    NativeGuard guard{};
    HRESULT monitor_query{E_PENDING}, history_query{E_PENDING};
    DWORD first_format{}, native_count{}, native_error{}, reacquisitions{}, user_objects{}, gdi_objects{}, handles{};
};
namespace win {
inline std::string_view recovery_guard_name(DWORD code) {
    constexpr std::string_view names[]{"none", "privacy-uncertain", "no-owner", "open-failed", "changed",
        "enum-failed", "first-not-unicode", "mixed", "too-many", "empty", "invalid-unicode", "recovered"};
    return code < sizeof(names) / sizeof(names[0]) ? names[code] : "unknown";
}
}
}
