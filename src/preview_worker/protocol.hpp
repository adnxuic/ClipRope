#pragma once
#include "application/screenshot_preview.hpp"
#include <windows.h>
#include <cstring>

namespace cliprope::preview_worker {
inline constexpr wchar_t container_name[] = L"cliprope.screenshotpreview.v1";
inline constexpr std::uint32_t magic = 0x31565053, version = 1;
inline constexpr size_t input_limit = 32u * 1024 * 1024, pixel_limit = 2u * 1024 * 1024;
struct Request {
    std::uint32_t magic_value = magic, protocol_version = version;
    std::uint64_t serial{};
    application::ScreenshotEncoding encoding{};
    std::uint32_t length{}, width{}, height{};
};
struct Response {
    std::uint32_t magic_value = magic, protocol_version = version;
    std::uint64_t serial{};
    std::uint32_t status = 1, width{}, height{}, stride{}, length{}, source_width{},
                  source_height{};
};
inline constexpr size_t output_size = sizeof(Response) + pixel_limit * 4;
inline UINT format(application::ScreenshotEncoding encoding) {
    using enum application::ScreenshotEncoding;
    return encoding == png     ? RegisterClipboardFormatW(L"PNG")
           : encoding == dib   ? CF_DIB
           : encoding == dibv5 ? CF_DIBV5
                               : 0;
}
inline bool valid(const Request &r) {
    return r.magic_value == magic && r.protocol_version == version && r.serial &&
           r.encoding >= application::ScreenshotEncoding::png &&
           r.encoding <= application::ScreenshotEncoding::dibv5 && r.length &&
           r.length <= input_limit && r.width && r.height && r.width <= 2048 && r.height <= 2048 &&
           std::uint64_t(r.width) * r.height <= pixel_limit;
}
inline bool valid(const Response &r, const Request &request) {
    return r.magic_value == magic && r.protocol_version == version && r.serial == request.serial &&
           !r.status && r.width && r.height && r.width <= request.width &&
           r.height <= request.height && r.width <= 2048 && r.height <= 2048 &&
           std::uint64_t(r.width) * r.height <= pixel_limit && r.stride == r.width * 4 &&
           r.length == std::uint64_t(r.stride) * r.height && r.source_width && r.source_height &&
           r.source_width <= 8192 && r.source_height <= 8192 &&
           std::uint64_t(r.source_width) * r.source_height <= 8u * 1024 * 1024 &&
           r.width <= r.source_width && r.height <= r.source_height;
}
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
struct Mapping {
    void *data{};
    Mapping(HANDLE h, DWORD access, size_t size) : data(MapViewOfFile(h, access, 0, 0, size)) {}
    ~Mapping() {
        if (data)
            UnmapViewOfFile(data);
    }
};
} // namespace cliprope::preview_worker
