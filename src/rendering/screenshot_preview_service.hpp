#pragma once
#include "application/screenshot_preview.hpp"
#include <memory>

namespace cliprope::rendering {
class ScreenshotPreviewService {
  public:
    enum class Status { ready, content_failure, sandbox_failure, backoff, shutdown_failure };
    using Resolve = std::function<application::ScreenshotSource(
        const domain::Entry &, std::stop_token, application::PreviewDeadline)>;
    struct Result {
        application::ScreenshotPreviewRequest request;
        std::shared_ptr<const domain::ImagePreview> image;
        Status status = Status::content_failure;
    };
    struct Counters {
        std::uint64_t requests{}, decodes{}, hits{}, failures{}, cancellations{};
        size_t bytes{};
        std::uint64_t shutdown_failures{};
        unsigned long last_termination_error{}, last_wait_error{};
        bool last_exit_confirmed{};
        std::uint64_t idle_wakes{};
    };
    // Ready only posts a notification; it runs off the UI thread and must not block.
    explicit ScreenshotPreviewService(Resolve, std::function<void()> ready = {});
    ~ScreenshotPreviewService();
    void request(application::ScreenshotPreviewRequest);
    void cancel(bool purge = false);
    std::optional<Result> take();
    Counters counters() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cliprope::rendering
