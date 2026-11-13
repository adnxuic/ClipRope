#pragma once
#include <windows.h>
#include "domain/image_preview.hpp"
#include <span>
#include <stop_token>
#include <functional>
#include <string>
#include <vector>

namespace cliprope::rendering {
using domain::ImagePreview;
// Decode only the built-in PNG codec or the checked packed DIB subset.
bool validate_png_pixels(std::span<const unsigned char> bytes);
// The default bounds cover a card's picture area at 200% scale.
inline constexpr unsigned preview_width = 320, preview_height = 256;
std::uint64_t preview_decodes();
void record_preview_decodes(unsigned count);
ImagePreview decode_preview(UINT format, std::span<const unsigned char> bytes, unsigned max_width = preview_width,
                            unsigned max_height = preview_height);
// Only the isolated screenshot worker calls this larger, independently bounded decoder.
ImagePreview decode_screenshot_preview(UINT format, std::span<const unsigned char> bytes, unsigned width, unsigned height);
std::vector<unsigned char> encode_dib_png(UINT format, std::span<const unsigned char> bytes);
using MetadataReady = std::function<void(domain::ImageMetadata)>;
ImagePreview decode_file_preview(const std::wstring& path, unsigned width, unsigned height, bool enlarge = false,
                                 const MetadataReady& measured = {});
bool matches_image_file(const std::wstring& path, UINT format, std::span<const unsigned char> bytes, std::stop_token stop = {});
} // namespace cliprope::rendering
