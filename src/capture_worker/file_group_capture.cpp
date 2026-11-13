#include "capture_worker/file_group_capture.hpp"
#include "platform_win/file_group.hpp"
#include <algorithm>

namespace cliprope {
void finalize_file_group_capture(CaptureResult& result) {
    const auto copy = copy_drop_effect();
    const auto has_group = std::any_of(result.saved.begin(), result.saved.end(),
        [](const auto& value) { return value.format == CF_HDROP; });
    auto match = [&](const auto& value) { return value.format == copy.format; };
    if (!has_group) {
        // A drop effect alone cannot supply file data.
        std::erase_if(result.saved, match);
        for (auto& row : result.formats)
            if (match(row) && row.outcome == "saved") row.outcome = "unsupported-orphan-effect";
        return;
    }
    const auto found = std::find_if(result.saved.begin(), result.saved.end(), match);
    if (found != result.saved.end()) *found = copy;
    else if (result.formats.size() < 256) {
        result.saved.push_back(copy);
        result.formats.push_back({copy.format, TYMED_HGLOBAL, DVASPECT_CONTENT, -1, "saved"});
    } else {
        std::erase_if(result.saved, [](const auto& item) { return item.format == CF_HDROP; });
        for (auto& row : result.formats)
            if (row.format == CF_HDROP && row.outcome == "saved") row.outcome = "effect-manifest-limit";
    }
}
} // namespace cliprope
