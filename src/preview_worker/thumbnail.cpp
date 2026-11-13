#include "preview_worker/thumbnail.hpp"
#include "preview_worker/thumbnail_store.hpp"
#include "preview_worker/protocol.hpp"
#include "cache/thumbnail_name.hpp"
#include "platform_win/file_group_paths.hpp"
#include "platform_win/image_file_access.hpp"
#include "rendering/image_preview.hpp"
#ifdef CLIPROPE_THUMBNAIL_PROBES
#include "thumbnail_gate.hpp"
#endif
#include <algorithm>
namespace cliprope::preview_worker {
namespace {
using namespace rendering;
FileState inspect(const std::vector<std::wstring>& paths, bool explicit_read) {
    using enum domain::FileAvailability;
    FileState state;
    state.availability = available;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto& path = paths[i];
        WIN32_FILE_ATTRIBUTE_DATA info{};
        const auto item = !explicit_read && win::needs_explicit_read(path) ? on_request : win::inspect_image_file(path, info);
        if (item != available) {
            if (state.availability == available || item == missing || (item == inaccessible && state.availability == on_request))
                state.availability = item;
            continue;
        }
        if (paths.size() == 1) state.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (i < 4) {
            state.version[i * 2] = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            state.version[i * 2 + 1] = (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) |
                                        info.ftLastWriteTime.dwLowDateTime;
        }
    }
    return state;
}
ImagePreview file_collage(const std::vector<std::wstring>& paths, unsigned width) {
    const unsigned height = width * preview_height / preview_width;
    const unsigned rows = paths.size() == 2 ? 1 : 2;
    const unsigned w = width / 2, h = height / rows;
    ImagePreview sheet{width, height, std::vector<BYTE>(width * height * 4)};
    for (unsigned i = 0; i < std::min<std::size_t>(4, paths.size()); ++i) {
        auto tile = decode_file_preview(paths[i], w - 2, h - 2, true);
        const auto left = (i % 2) * w + (w - tile.width) / 2;
        const auto top = (i / 2) * h + (h - tile.height) / 2;
        for (unsigned y = 0; y < tile.height; ++y)
            std::memcpy(sheet.pbgra.data() + ((top + y) * sheet.width + left) * 4,
                        tile.pbgra.data() + y * tile.width * 4, tile.width * 4);
    }
    return sheet;
}
} // namespace

int serve_thumbnail(HANDLE input, HANDLE output, HANDLE metadata, size_t size) {
    if (!size || size > 32u * 1024 * 1024) return 2;
    Mapping in(input, FILE_MAP_READ, sizeof(ThumbnailRequest) + size);
    if (!in.data) return 2;
    ThumbnailRequest request;
    std::memcpy(&request, in.data, sizeof request);
    if (!valid_thumbnail_request(request) || request.bytes != size || request.deadline > GetTickCount64() + 3000)
        return 2;
    Mapping out(output, FILE_MAP_WRITE, thumbnail_output_size(request));
    if (!out.data) return 2;
    auto* result = static_cast<ThumbnailResponse*>(out.data);
    *result = ThumbnailResponse{};
    result->generation = request.generation;
    if (GetTickCount64() >= request.deadline) { result->status = ThumbnailStatus::expired; return 0; }
    if (drag_encoding(request.op)) {
        try {
            const auto* pixels = static_cast<const BYTE*>(in.data) + sizeof request;
            auto png = encode_dib_png(request.op == ThumbnailOp::drag_dib ? CF_DIB : CF_DIBV5, {pixels, size});
            if (png.size() > thumbnail_output_size(request) - sizeof *result) return 2;
            std::memcpy(result + 1, png.data(), png.size());
            result->encoded_bytes = static_cast<DWORD>(png.size());
            result->status = GetTickCount64() >= request.deadline ? ThumbnailStatus::expired : ThumbnailStatus::complete;
        } catch (const std::exception&) { result->status = ThumbnailStatus::decode_failed; }
        return 0;
    }
    const auto* root = reinterpret_cast<const wchar_t*>(static_cast<const BYTE*>(in.data) + sizeof request);
    const std::wstring profile(root, size / sizeof(wchar_t));
    if (profile.find(L'\0') != profile.npos) return 2;
#ifdef CLIPROPE_THUMBNAIL_PROBES
    thumbnail_fixture::owner_pid = request.host_pid;
#endif
    try {
        ThumbnailStore store(profile, request);
        if (!store.reachable()) { result->status = ThumbnailStatus::complete; return 0; }
        ImagePreview image;
        const auto width = request.width;
        const auto height = (width * 9 + 10) / 11;
        if (request.op == ThumbnailOp::file_picture || request.op == ThumbnailOp::file_status) {
            const auto paths = file_group_paths(store.load());
            if (paths.empty() || (request.op == ThumbnailOp::file_picture && !image_file_group(paths))) {
                result->status = ThumbnailStatus::invalid;
                return 0;
            }
#ifdef CLIPROPE_THUMBNAIL_PROBES
            thumbnail_fixture::pause_at(L"inspect");
#endif
            const auto state = inspect(paths, request.explicit_read != 0);
            result->file = state;
            const bool available = state.availability == domain::FileAvailability::available;
            if (request.op == ThumbnailOp::file_picture && state.directory != 1) {
                if (available && request.use_version && state.version == request.known_version)
                    result->reuse = 1;
                else if (!available || !request.refresh)
                    image = store.cached(&state.version, !available);
            }
            if (request.op == ThumbnailOp::file_picture && available && state.directory != 1 &&
                !result->reuse && image.pbgra.empty()) {
                if (paths.size() == 1) {
                    result->decodes = 1;
                    image = decode_file_preview(paths.front(), width, height, false, [&](auto source) {
                        source.version = state.version[0] ^ state.version[1];
                        result->source = source;
                        SetEvent(metadata);
                    });
                    image.source = result->source;
                } else {
                    result->decodes = static_cast<unsigned>(std::min<size_t>(4, paths.size()));
                    image = file_collage(paths, width);
                }
                if (inspect(paths, request.explicit_read != 0) != state)
                    throw std::runtime_error("File changed during preview");
#ifdef CLIPROPE_THUMBNAIL_PROBES
                thumbnail_fixture::pause_at(L"install");
#endif
                store.install(image, &state.version);
            }
        } else {
            const UINT format = request.op == ThumbnailOp::png ? RegisterClipboardFormatW(L"PNG") :
                                request.op == ThumbnailOp::dib ? CF_DIB : CF_DIBV5;
            image = store.cached(nullptr);
            if (image.pbgra.empty()) {
                result->decodes = 1;
                image = decode_preview(format, store.load(), width, height);
                store.install(image, nullptr);
            }
        }
        if (image.pbgra.size() > thumbnail_output_size(request) - sizeof *result)
            throw std::runtime_error("Thumbnail exceeds output section");
        result->width = image.width;
        result->height = image.height;
        if (!image.pbgra.empty()) {
            // Single-file metadata was published before signaling the event. Do not rewrite it.
            if (!result->decodes || request.op != ThumbnailOp::file_picture) result->source = image.source;
            std::memcpy(result + 1, image.pbgra.data(), image.pbgra.size());
        }
        result->live = store.reachable();
        result->status = GetTickCount64() >= request.deadline ? ThumbnailStatus::expired : ThumbnailStatus::complete;
    } catch (const persistence::StorageFailure& failure) {
        result->status = failure.reason() == persistence::StorageProblem::unsupported_format
            ? ThumbnailStatus::unsupported_format : ThumbnailStatus::decode_failed;
        result->live = result->status == ThumbnailStatus::decode_failed;
    } catch (const std::exception&) {
        result->status = ThumbnailStatus::decode_failed;
        result->live = 1; // Cache the failure briefly. No derivative is installed on this path.
    }
    return 0;
}
} // namespace cliprope::preview_worker
