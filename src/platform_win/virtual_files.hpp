#pragma once
#include "platform_win/clipboard.hpp"
#include <shlobj.h>

namespace cliprope::virtual_files {
inline constexpr std::size_t file_limit = 16, stream_limit = 8u * 1024u * 1024u, group_limit = 32u * 1024u * 1024u;
UINT descriptor_format();
UINT contents_format();
bool validate_descriptors(Representation& group);
std::vector<FILEDESCRIPTORW> descriptors(const Representation& group);
// Extend a validated pixel-only publication. A returned index supplies FileContents from the original PNG.
std::optional<std::size_t> append_screenshot(std::vector<Representation>& formats);
bool complete(const std::vector<Representation>& formats);
} // namespace cliprope::virtual_files
