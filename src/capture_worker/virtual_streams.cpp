#include "capture_worker/virtual_streams.hpp"
#include "capture_worker/stream_reader.hpp"
#include "platform_win/virtual_files.hpp"
#include <algorithm>

namespace cliprope {
void capture_virtual_streams(IDataObject& source, CaptureResult& result, const std::function<bool()>& superseded) {
    const auto desc = virtual_files::descriptor_format(), content = virtual_files::contents_format();
    if (std::any_of(result.saved.begin(), result.saved.end(), [](const auto& r) { return r.format == CF_HDROP; })) {
        std::erase_if(result.saved, [&](const auto& r) { return r.format == desc || r.format == content; });
        for (auto& row : result.formats) if (row.format == desc || row.format == content) row.outcome = "unsupported-path-companion";
        return;
    }
    const auto group = std::find_if(result.saved.begin(), result.saved.end(), [&](const auto& item) { return item.format == desc; });
    if (group == result.saved.end()) {
        for (auto& row : result.formats) if (row.outcome == "indexed-offer") row.outcome = "orphan-virtual-content";
        return;
    }
    // Shell producers may enumerate a single index -1 wildcard. The validated
    // descriptors define the real, zero-based requests. Read every file once.
    const auto files = virtual_files::descriptors(*group);
    std::erase_if(result.formats, [](const auto& row) { return row.outcome == "indexed-offer"; });
    std::size_t total{};
    for (const auto& item : result.saved) total += item.bytes.size();
    const auto available = std::min(virtual_files::group_limit, snapshot_byte_limit - total);
    std::size_t used{};
    bool complete = !files.empty() && result.formats.size() + files.size() <= 256;
    if (complete) {
        for (size_t i = 0; i < files.size() && !superseded(); ++i) {
            Representation item{content, {}, static_cast<LONG>(i), TYMED_ISTREAM};
            std::optional<std::uint64_t> size;
            if (files[i].dwFlags & FD_FILESIZE)
                size = (static_cast<std::uint64_t>(files[i].nFileSizeHigh) << 32) | files[i].nFileSizeLow;
            auto status = read_bounded_stream(source, item, std::min(virtual_files::stream_limit, available - used),
                                              size, result.error, superseded);
            result.formats.push_back({content, TYMED_ISTREAM, DVASPECT_CONTENT, item.index, status});
            if (status == "saved") {
                used += item.bytes.size();
                result.saved.push_back(std::move(item));
            } else complete = false;
        }
    }
    if (!complete || superseded()) {
        std::erase_if(result.saved, [&](const auto& item) { return item.format == desc || item.format == content; });
        for (auto& row : result.formats)
            if ((row.format == desc || row.format == content) && row.outcome == "saved") row.outcome = "incomplete-virtual-group";
    }
}
} // namespace cliprope
