#pragma once
#include "application/rope_changes.hpp"
#include "platform_win/unicode_name.hpp"

namespace cliprope::win {
inline std::string edited_rope_name(std::wstring_view input) {
    try { return unicode::utf8(input); }
    catch (const std::invalid_argument&) { throw application::InvalidRopeName{}; }
}
}
