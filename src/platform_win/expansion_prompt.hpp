#pragma once
#include "application/expand_file_group.hpp"
#include <windows.h>

namespace cliprope::win {
bool review_file_group_expansion(HWND owner, const std::wstring& rope_name, std::size_t member_count,
                                 const application::ExpansionImpact& impact, bool keep_source);
}
