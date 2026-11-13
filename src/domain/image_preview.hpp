#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
namespace cliprope::domain {
struct ImageMetadata {
    unsigned width{}, height{}; // Source pixels after applying orientation.
    unsigned orientation{};    // Clockwise quarter turns and mirror flags (WIC convention).
    std::uint64_t version{};
    bool operator==(const ImageMetadata&) const = default;
};
enum class ImageSizeMode { compact, standard, large };
struct ImagePreferences {
    ImageSizeMode size = ImageSizeMode::standard;
    bool checkerboard{};
    bool operator==(const ImagePreferences&) const = default;
};
struct ImagePreview {
    unsigned width{}, height{};
    std::vector<unsigned char> pbgra;
    ImageMetadata source;
};
enum class FileAvailability { unchecked, available, missing, inaccessible, on_request, decode_failed };
struct FilePreview {
    std::shared_ptr<const ImagePreview> image;
    FileAvailability availability = FileAvailability::unchecked;
    std::optional<bool> directory;
    ImageMetadata source;
};
} // namespace cliprope::domain
