#pragma once

#include "platform_win/clipboard.hpp"
#include "platform_win/file_group_paths.hpp"
#include "application/expand_file_group.hpp"

namespace cliprope {

application::ExpandFileGroup inspect_expansion(application::ExpandFileGroup group);
bool is_file_group_format(UINT format);
bool validate_file_group(Representation& value);
std::vector<std::wstring> file_group_paths(const Representation& value);

bool file_group_available(const Representation& value);
Representation copy_drop_effect();
bool prepare_file_group(std::vector<Representation>& saved);
} // namespace cliprope
