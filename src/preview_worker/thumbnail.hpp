#pragma once
#include "domain/image_preview.hpp"
#include <windows.h>
#include <array>
#include <functional>
#include <filesystem>
#include <span>
#include <stop_token>

namespace cliprope::preview_worker {
enum class ThumbnailOp : std::uint32_t { file_status, file_picture, png, dib, dibv5, drag_dib, drag_dibv5 };
inline bool drag_encoding(ThumbnailOp op) { return op == ThumbnailOp::drag_dib || op == ThumbnailOp::drag_dibv5; }
using FileVersion = std::array<std::uint64_t, 8>;
struct FileState {
    domain::FileAvailability availability = domain::FileAvailability::unchecked;
    FileVersion version{};
    int directory = -1;
    bool operator==(const FileState&) const = default;
};
struct ThumbnailRequest {
    std::uint32_t signature = 0x31524854, protocol = 2;
    ThumbnailOp op{};
    std::uint32_t width{}, bytes{}, explicit_read{}, use_version{}, refresh{}, host_pid{};
    std::array<char, 64> hash{};
    std::uint64_t generation{}, deadline{};
    FileVersion known_version{};
};
enum class ThumbnailStatus : std::uint32_t { pending, complete, invalid, decode_failed, expired, unsupported_format };
struct ThumbnailResponse {
    std::uint32_t signature = 0x32534854;
    ThumbnailStatus status{};
    std::uint64_t generation{};
    FileState file;
    std::uint32_t width{}, height{}, decodes{}, live{}, reuse{}, encoded_bytes{};
    domain::ImageMetadata source{};
};
struct ThumbnailResult {
    FileState file;
    domain::ImagePreview image;
    unsigned decodes{};
    bool live{}, reuse{};
    std::vector<unsigned char> encoded;
};
std::vector<unsigned char> encode_drag_png(UINT format, std::span<const unsigned char> pixels,
                                           std::uint64_t serial, std::stop_token);
bool valid_thumbnail_request(const ThumbnailRequest&);
size_t thumbnail_output_size(const ThumbnailRequest&);
ThumbnailResult run_thumbnail(ThumbnailRequest, const std::filesystem::path&, std::stop_token,
                              const std::function<void(domain::ImageMetadata)>& measured);
int serve_thumbnail(HANDLE input, HANDLE output, HANDLE metadata, size_t size);
} // namespace cliprope::preview_worker
