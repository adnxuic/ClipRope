#pragma once
#include "application/browse.hpp"
#include <chrono>
#include <functional>
#include <stop_token>

namespace cliprope::application {
using PreviewDeadline = std::chrono::steady_clock::time_point;
enum class ScreenshotEncoding : std::uint32_t { png = 1, dib = 2, dibv5 = 3 };
struct ScreenshotPreviewRequest {
    domain::Entry entry;
    std::uint64_t owner_generation{}, serial{};
    unsigned width{}, height{};
};
struct ScreenshotSource {
    ScreenshotEncoding encoding{};
    std::string hash;
    std::size_t length{};
    std::shared_ptr<void> lease;
    std::function<std::vector<unsigned char>(std::stop_token, PreviewDeadline)> read;
};
inline bool screenshot_preview_eligible(const CardSummary &card) {
    return card.menu == CardMenuKind::screenshot && card.image_primary &&
           card.file_list_hash.empty() && card.status != CardStatus::damaged;
}
inline bool preview_expired(std::stop_token stop, PreviewDeadline deadline) {
    return stop.stop_requested() || std::chrono::steady_clock::now() >= deadline;
}
} // namespace cliprope::application
