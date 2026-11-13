#include "preview_worker/thumbnail.hpp"
#include "cache/thumbnail_name.hpp"

namespace cliprope::preview_worker {
bool valid_thumbnail_request(const ThumbnailRequest& r) {
    if (drag_encoding(r.op))
        return r.signature == 0x31524854 && r.protocol == 2 && r.host_pid && r.generation && r.deadline &&
               r.bytes >= 40 && r.bytes <= 32u * 1024 * 1024 && !r.width && !r.explicit_read && !r.use_version &&
               !r.refresh && r.hash == decltype(r.hash){} && r.known_version == FileVersion{};
    const bool file = r.op == ThumbnailOp::file_status || r.op == ThumbnailOp::file_picture;
    return r.signature == 0x31524854 && r.protocol == 2 && r.op <= ThumbnailOp::dibv5 &&
           r.host_pid && r.generation && r.deadline && r.bytes &&
           r.bytes % sizeof(wchar_t) == 0 && r.bytes <= 32767 * sizeof(wchar_t) &&
           cache::thumbnail_hash({r.hash.data(), r.hash.size()}) && r.refresh <= 1 && r.explicit_read <= 1 &&
           r.use_version <= 1 && (file || (!r.explicit_read && !r.use_version && !r.refresh)) &&
           std::binary_search(cache::thumbnail_widths.begin(), cache::thumbnail_widths.end(), r.width);
}

size_t thumbnail_output_size(const ThumbnailRequest& r) {
    if (drag_encoding(r.op))
        return sizeof(ThumbnailResponse) + std::min<size_t>(33u * 1024 * 1024, size_t(r.bytes) * 2 + 1024 * 1024);
    return sizeof(ThumbnailResponse) +
           (r.op == ThumbnailOp::file_status ? 0 : r.op == ThumbnailOp::file_picture ?
            size_t(1024) * 1024 * 4 : size_t(r.width) * r.width * 4);
}

} // namespace cliprope::preview_worker
