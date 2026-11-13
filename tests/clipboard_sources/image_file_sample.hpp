#pragma once
#include "platform_win/clipboard.hpp"
#include <shlobj.h>
#include <cstring>
#include <filesystem>

namespace image_file_sample {
inline cliprope::Representation drop(const std::vector<std::filesystem::path>& paths) {
    DROPFILES header{};
    header.pFiles = sizeof header;
    header.fWide = TRUE;
    cliprope::Representation value{CF_HDROP, std::vector<unsigned char>(sizeof header)};
    std::memcpy(value.bytes.data(), &header, sizeof header);
    for (const auto& path : paths) {
        const auto* first = reinterpret_cast<const unsigned char*>(path.c_str());
        value.bytes.insert(value.bytes.end(), first, first + (path.native().size() + 1) * sizeof(wchar_t));
    }
    value.bytes.resize(value.bytes.size() + sizeof(wchar_t));
    return value;
}
} // namespace image_file_sample
