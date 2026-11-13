#pragma once
#include <windows.h>
#include <optional>
#include <span>
#include <vector>
#include "domain/image_preview.hpp"

namespace cliprope::media {
bool supports(UINT format);
std::size_t byte_limit(UINT format);
// PNG validation reads every decoded row and requires an initialized COM apartment.
bool validate(UINT format, std::vector<unsigned char>& bytes);
struct Dib {
    unsigned width, height, bits, stride;
    std::size_t offset, size;
    bool bottom_up, alpha;
};
std::optional<Dib> inspect_dib(UINT format, std::span<const unsigned char> bytes);
// Header-only validation: never invokes a codec in the parent process.
std::optional<domain::ImageMetadata> screenshot_header(UINT format, std::span<const unsigned char> bytes);
inline constexpr unsigned max_side = 8192;
inline constexpr std::size_t max_pixels = 8u * 1024u * 1024u;
} // namespace cliprope::media
