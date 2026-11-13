#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cliprope {
inline constexpr std::size_t max_group_bytes = 4u * 1024u * 1024u;
std::vector<unsigned char> copy_effect_bytes();
bool disk_path(std::wstring_view name);
std::vector<unsigned char> encode_file_group(const std::vector<std::wstring>& paths);
std::vector<std::wstring> file_group_paths(std::span<const unsigned char> bytes);
bool image_file_group(const std::vector<std::wstring>& paths);
}
