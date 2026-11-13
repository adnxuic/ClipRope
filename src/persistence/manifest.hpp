#pragma once
#include "domain/history.hpp"
#include "persistence/database.hpp"

namespace cliprope::persistence {
inline constexpr size_t max_blob = 64 * 1024 * 1024;
inline constexpr size_t max_snapshot = 128 * 1024 * 1024;
inline constexpr size_t max_formats = 256;

// The first eight result columns are standard_id, format_name, tymed, aspect,
// lindex, codec_version, priority and status, in that order.
domain::Format read_manifest_format(const Statement& row);
void append_manifest(std::vector<unsigned char>& bytes, const domain::Format&, std::string_view blob);
void verify_manifest(std::span<const unsigned char> bytes, std::string_view seal);
}
