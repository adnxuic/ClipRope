#include "rendering/screenshot_preview_service.hpp"
#include "preview_worker/process.hpp"
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <algorithm>
#include <utility>

namespace cliprope::rendering {
using namespace application;
using Clock = std::chrono::steady_clock;
struct ScreenshotPreviewService::Impl {
    struct Job {
        ScreenshotPreviewRequest request;
        std::stop_source stop;
        PreviewDeadline deadline;
        std::uint64_t epoch;
    };
    struct Cached {
        std::shared_ptr<const domain::ImagePreview> pixels;
        std::uint64_t used;
    };
    Resolve resolve;
    std::function<void()> ready;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::optional<Job> pending;
    std::stop_source active;
    std::optional<Result> completed;
    std::map<std::string, Cached> cache;
    std::map<std::string, PreviewDeadline> failed;
    Counters counts;
    PreviewDeadline touched = Clock::now(), backoff{};
    std::uint64_t epoch{}, tick{};
    bool stop{}, engaged{};
    Impl(Resolve r, std::function<void()> notify)
        : resolve(std::move(r)), ready(std::move(notify)) {}
    ~Impl() {
        {
            std::lock_guard guard(mutex);
            stop = true;
            active.request_stop();
            pending.reset();
            completed.reset();
        }
        wake.notify_one();
        if (worker.joinable())
            worker.join();
    }
    void clear() {
        cache.clear();
        failed.clear();
        counts.bytes = 0;
    }
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                while (!stop && !pending) {
                    if (engaged || (cache.empty() && failed.empty())) {
                        wake.wait(lock);
                    } else if (wake.wait_until(lock, touched + std::chrono::seconds(30)) == std::cv_status::timeout) {
                        ++counts.idle_wakes;
                        if (!engaged && Clock::now() >= touched + std::chrono::seconds(30)) clear();
                    }
                }
                if (stop)
                    return;
                job = std::move(*pending);
                pending.reset();
                active = job.stop;
            }
            const auto token = job.stop.get_token();
            std::shared_ptr<const domain::ImagePreview> pixels;
            std::string key;
            bool sandbox_failed = false;
            auto status = Status::content_failure;
            try {
                {
                    std::lock_guard guard(mutex);
                    if (Clock::now() < backoff) {
                        status = Status::backoff;
                        throw std::runtime_error("Preview backoff");
                    }
                }
                auto source = resolve(job.request.entry, token, job.deadline);
                key = source.hash + ":" + std::to_string(static_cast<unsigned>(source.encoding)) +
                      ":" + std::to_string(job.request.width) + ":" +
                      std::to_string(job.request.height) + ":1";
                {
                    std::lock_guard guard(mutex);
                    if (job.epoch != epoch || preview_expired(token, job.deadline))
                        continue;
                    if (auto it = failed.find(key); it != failed.end() && Clock::now() < it->second)
                        throw std::runtime_error("Preview failure cached");
                    if (auto it = cache.find(key); it != cache.end()) {
                        it->second.used = ++tick;
                        pixels = it->second.pixels;
                        ++counts.hits;
                    }
                }
                if (!pixels) {
                    auto bytes = source.read(token, job.deadline);
                    if (preview_expired(token, job.deadline))
                        continue;
                    preview_worker::Request wire;
                    wire.serial = job.request.serial;
                    wire.encoding = source.encoding;
                    wire.length = static_cast<unsigned>(bytes.size());
                    wire.width = job.request.width;
                    wire.height = job.request.height;
                    {
                        std::lock_guard guard(mutex);
                        ++counts.decodes;
                    }
                    auto image = preview_worker::decode(wire, bytes, token, job.deadline);
                    pixels = std::make_shared<domain::ImagePreview>(std::move(image));
                }
            } catch (const preview_worker::ShutdownFailure &failure) {
                std::lock_guard guard(mutex);
                ++counts.shutdown_failures;
                counts.last_termination_error = failure.termination_error;
                counts.last_wait_error = failure.wait_error;
                counts.last_exit_confirmed = failure.confirmed_exit;
                backoff = Clock::now() + std::chrono::seconds(30);
                status = Status::shutdown_failure;
            } catch (const preview_worker::SandboxFailure &) {
                sandbox_failed = true;
                status = Status::sandbox_failure;
            } catch (const std::exception &) {
            }
            std::unique_lock guard(mutex);
            if (stop)
                return;
            if (sandbox_failed && !token.stop_requested())
                backoff = Clock::now() + std::chrono::seconds(30);
            if (job.epoch != epoch || preview_expired(token, job.deadline)) {
                ++counts.cancellations;
                continue;
            }
            if (pixels) {
                constexpr size_t budget = 16u * 1024 * 1024;
                if (!cache.contains(key)) {
                    while (!cache.empty() &&
                           (counts.bytes + pixels->pbgra.size() > budget || cache.size() >= 64)) {
                        auto oldest = std::min_element(cache.begin(), cache.end(),
                                                       [](const auto &a, const auto &b) {
                                                           return a.second.used < b.second.used;
                                                       });
                        counts.bytes -= oldest->second.pixels->pbgra.size();
                        cache.erase(oldest);
                    }
                    counts.bytes += pixels->pbgra.size();
                    cache.emplace(key, Cached{pixels, ++tick});
                }
            } else {
                ++counts.failures;
                engaged = false;
                touched = Clock::now();
                if (!key.empty()) {
                    if (failed.size() >= 64)
                        failed.erase(failed.begin());
                    failed[key] = Clock::now() + std::chrono::seconds(30);
                }
            }
            if (pixels)
                status = Status::ready;
            completed = Result{job.request, std::move(pixels), status};
            guard.unlock();
            if (ready) {
                try {
                    ready();
                } catch (...) {
                }
            }
        }
    }
};
ScreenshotPreviewService::ScreenshotPreviewService(Resolve resolve, std::function<void()> ready)
    : impl_(std::make_unique<Impl>(std::move(resolve), std::move(ready))) {}
ScreenshotPreviewService::~ScreenshotPreviewService() = default;
void ScreenshotPreviewService::request(ScreenshotPreviewRequest request) {
    std::lock_guard guard(impl_->mutex);
    impl_->active.request_stop();
    impl_->completed.reset();
    impl_->touched = Clock::now();
    impl_->engaged = true;
    ++impl_->counts.requests;
    impl_->pending =
        Impl::Job{std::move(request), {}, Clock::now() + std::chrono::seconds(2), ++impl_->epoch};
    if (!impl_->worker.joinable())
        impl_->worker = std::thread([p = impl_.get()] { p->run(); });
    impl_->wake.notify_one();
}
void ScreenshotPreviewService::cancel(bool purge) {
    std::lock_guard guard(impl_->mutex);
    ++impl_->epoch;
    impl_->active.request_stop();
    impl_->pending.reset();
    impl_->completed.reset();
    impl_->touched = Clock::now();
    impl_->engaged = false;
    if (purge)
        impl_->clear();
    impl_->wake.notify_one();
}
std::optional<ScreenshotPreviewService::Result> ScreenshotPreviewService::take() {
    std::lock_guard guard(impl_->mutex);
    return std::exchange(impl_->completed, {});
}
ScreenshotPreviewService::Counters ScreenshotPreviewService::counters() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->counts;
}
} // namespace cliprope::rendering
