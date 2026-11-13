#pragma once
#include "application/preview_changes.hpp"
#include "rendering/image_preview.hpp"
#include <filesystem>
#include <functional>
#include <memory>

namespace cliprope::rendering {
class ThumbnailCache {
  public:
    explicit ThumbnailCache(std::filesystem::path profile);
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;
    // Only a cache hit returns pixels immediately. Decode and disk I/O run off the UI thread.
    std::shared_ptr<const ImagePreview> request(std::string hash, UINT format, unsigned width = preview_width);
    domain::FilePreview request_files(std::string hash, unsigned width, bool picture = true);
    void recheck_files(std::string requested_list = {});
    void on_ready(std::function<void()> callback);
    application::PreviewChanges take_changes();
    std::size_t bytes() const;
    static constexpr std::size_t budget = 16u * 1024u * 1024u;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace cliprope::rendering
