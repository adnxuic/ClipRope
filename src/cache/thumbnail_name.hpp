#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace cliprope::cache {
inline constexpr std::array thumbnail_widths{80u, 160u, 240u, 320u, 400u, 480u, 560u, 640u, 720u, 800u, 880u, 960u, 1024u};
inline constexpr std::string_view png = "png", dib = "dib", dibv5 = "dibv5", files = "files";
inline constexpr std::string_view extension = ".thumb", staging_extension = ".tmp";

inline unsigned thumbnail_width(unsigned requested) {
    return *std::lower_bound(thumbnail_widths.begin(), thumbnail_widths.end(), std::min(requested, thumbnail_widths.back()));
}

inline bool thumbnail_hash(std::string_view hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == hash.npos;
}

inline std::string thumbnail_key(std::string_view hash, std::string_view adapter, unsigned width) {
    std::string key(hash);
    key.append("-").append(adapter);
    // The original 320-pixel image cache used an unsuffixed filename.
    if (adapter == files || width != 320) key += "-" + std::to_string(width);
    return key;
}

struct ThumbnailName {
    std::string_view hash;
    bool file_list;
    bool staged;
};

// Views refer to the caller's filename. Only names owned by these adapters are reclaimable.
inline std::optional<ThumbnailName> parse_thumbnail_name(std::string_view name) {
    const bool staged = name.ends_with(staging_extension);
    if (staged) name.remove_suffix(staging_extension.size());
    if (!name.ends_with(extension)) return {};
    name.remove_suffix(extension.size());
    const auto dash = name.find('-');
    const auto hash = name.substr(0, dash);
    if (dash == name.npos || !thumbnail_hash(hash)) return {};
    name.remove_prefix(dash + 1);
    const auto separator = name.find('-');
    const auto adapter = name.substr(0, separator);
    if (adapter != files && adapter != png && adapter != dib && adapter != dibv5) return {};
    if (separator != name.npos) {
        auto digits = name.substr(separator + 1);
        if (digits.empty() || digits.front() == '0') return {};
        unsigned width{};
        const auto end = digits.data() + digits.size();
        const auto parsed = std::from_chars(digits.data(), end, width);
        if (parsed.ec != std::errc{} || parsed.ptr != end ||
            !std::binary_search(thumbnail_widths.begin(), thumbnail_widths.end(), width)) return {};
    } else if (adapter == files) return {};
    return ThumbnailName{hash, adapter == files, staged};
}
} // namespace cliprope::cache
