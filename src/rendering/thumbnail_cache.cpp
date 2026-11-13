#include "rendering/thumbnail_cache.hpp"
#include "cache/thumbnail_name.hpp"
#ifdef CLIPROPE_THUMBNAIL_PROBES
#include "thumbnail_gate.hpp"
#endif
#include "preview_worker/thumbnail.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace cliprope::rendering {
namespace fs = std::filesystem;
using preview_worker::FileState;
using preview_worker::ThumbnailOp;
struct ThumbnailCache::Impl : std::enable_shared_from_this<Impl> {
    struct Job {
        std::string key;
        ThumbnailOp op;
        std::uint64_t generation{};
        unsigned width{};
        bool explicit_read{};
        std::uint64_t refresh{};
        bool file() const { return op == ThumbnailOp::file_status || op == ThumbnailOp::file_picture; }
    };
    struct Cached {
        std::shared_ptr<const ImagePreview> image;
        std::uint64_t last;
        domain::FileAvailability availability{};
        std::uint64_t expires{}, generation{};
        std::optional<bool> directory;
        std::optional<FileState> state;
        domain::ImageMetadata source;
    };
    fs::path profile;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool running{};
    std::stop_source cancel;
    std::uint64_t serial{};
    std::deque<Job> queue;
    std::set<std::string> pending;
    std::map<std::string, Cached> cached;
    std::map<std::string, domain::ImageMetadata> measuring;
    std::function<void()> notify;
    application::PreviewChanges changes;
    void ready(const std::string& key) {
        if (!notify) return;
        changes.add(std::string_view(key).substr(0, 64), generation);
        notify();
    }
    std::size_t used = 0;
    std::uint64_t tick = 0;
    std::uint64_t generation = 1;
    // Each visible size must rebuild after an explicit refresh, even if two source
    // files have the same size and modification time. Keep concurrent requests separate.
    struct Refresh { std::uint64_t ticket, until; };
    std::map<std::string, Refresh> refreshes;
    std::uint64_t next_refresh{};
    bool stop = false;
    explicit Impl(fs::path path) : profile(std::move(path)) {}
    void shutdown() {
        std::unique_lock guard(mutex);
        stop = true;
        notify = {};
        queue.clear();
        cached.clear();
        measuring.clear();
        cancel.request_stop();
        // The callback owns only immutable request values, module-local state and its child Job.
        // A slow kernel process exit never holds the store or a window past this wait.
        wake.wait_for(guard, std::chrono::milliseconds(350), [&] { return !running; });
    }
    static void CALLBACK dispatch(PTP_CALLBACK_INSTANCE, void* context) noexcept {
        std::unique_ptr<std::shared_ptr<Impl>> owner(static_cast<std::shared_ptr<Impl>*>(context));
        auto& self = **owner;
        try { self.run(); }
        catch (...) {
            std::lock_guard guard(self.mutex);
            self.pending.clear();
            self.queue.clear();
            self.running = false;
            self.wake.notify_all();
        }
    }
    bool enqueue(Job job) {
        if (stop || pending.contains(job.key) || queue.size() >= 16) return false;
        const auto key = job.key;
        pending.insert(key);
        try {
            queue.push_back(std::move(job));
            if (!running) {
                auto context = std::make_unique<std::shared_ptr<Impl>>(shared_from_this());
                if (!TrySubmitThreadpoolCallback(dispatch, context.get(), nullptr)) {
                    queue.pop_back();
                    pending.erase(key);
                    return false;
                }
                context.release();
                running = true;
            }
        } catch (...) {
            if (!queue.empty() && queue.back().key == key) queue.pop_back();
            pending.erase(key);
            throw;
        }
        return true;
    }
    void run() {
        for (;;) {
            Job job;
            preview_worker::ThumbnailRequest request;
            std::shared_ptr<const ImagePreview> pixels;
            {
                std::lock_guard guard(mutex);
                if (stop || queue.empty()) {
                    running = false;
                    wake.notify_all();
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
                request.generation = ++serial;
                const auto old = cached.find(job.key);
                if (job.file() && !job.refresh && old != cached.end() && old->second.generation == job.generation &&
                    old->second.image && old->second.state) {
                    pixels = old->second.image;
                    request.known_version = old->second.state->version;
                    request.use_version = 1;
                }
            }
            request.op = job.op;
            request.width = job.width;
            request.explicit_read = job.explicit_read;
            request.refresh = job.refresh != 0;
            std::copy_n(job.key.begin(), request.hash.size(), request.hash.begin());
            domain::FileAvailability availability{};
            std::optional<bool> directory_hint;
            bool live = false;
            bool reused = false;
            std::optional<FileState> inspected;
            domain::ImageMetadata measured;
            try {
                auto result = preview_worker::run_thumbnail(request, profile, cancel.get_token(),
                    [&](domain::ImageMetadata source) {
                        std::lock_guard guard(mutex);
                        const auto refresh = refreshes.find(job.key);
                        if (!stop && job.generation == generation && (refresh == refreshes.end() ||
                            refresh->second.ticket == job.refresh)) {
                            measuring[job.key] = source;
                            ready(job.key);
                        }
                    });
                record_preview_decodes(result.decodes);
                if (!result.reuse) pixels.reset();
                if (!result.image.pbgra.empty()) pixels = std::make_shared<ImagePreview>(std::move(result.image));
                if (pixels) measured = pixels->source;
                live = result.live;
                reused = result.reuse;
                if (job.file()) {
                    inspected = result.file;
                    availability = result.file.availability;
                    if (result.file.directory != -1) directory_hint = result.file.directory != 0;
                }
            } catch (const std::exception&) {
                pixels.reset();
                // A missing helper or a deadline must not spin on every repaint.
                availability = domain::FileAvailability::decode_failed;
                live = true;
            }
            std::lock_guard guard(mutex);
            pending.erase(job.key);
            measuring.erase(job.key);
            if (stop) continue;
            auto old = cached.find(job.key);
            if (live && old != cached.end() && !job.refresh && !refreshes.contains(job.key) &&
                job.generation == generation && old->second.generation == generation &&
                old->second.state == inspected && (reused || job.op == ThumbnailOp::file_status)) {
                old->second.expires = GetTickCount64() + 2000;
                continue;
            }
            if (old != cached.end()) {
                if (availability == domain::FileAvailability::on_request && !job.explicit_read &&
                    old->second.availability != domain::FileAvailability::unchecked) {
                    availability = old->second.availability;
                    directory_hint = old->second.directory;
                }
                if (old->second.image) used -= old->second.image->pbgra.size();
                cached.erase(old);
            }
            auto requested = refreshes.find(job.key);
            if (!live || (job.file() && (job.generation != generation ||
                (requested != refreshes.end() && job.refresh != requested->second.ticket)))) {
#ifdef CLIPROPE_THUMBNAIL_PROBES
                if (job.op == ThumbnailOp::file_picture && job.generation != generation) thumbnail_fixture::report_discard();
#endif
                ready(job.key);
                continue;
            }
            if (requested != refreshes.end()) refreshes.erase(requested);
            const auto cost = pixels ? pixels->pbgra.size() : 0;
            while (!cached.empty() && (used + cost > budget || cached.size() >= 512)) {
                auto oldest =
                    std::min_element(cached.begin(), cached.end(), [](const auto& a, const auto& b) {
                        return a.second.last < b.second.last;
                    });
                if (oldest->second.image)
                    used -= oldest->second.image->pbgra.size();
                cached.erase(oldest);
            }
            cached.emplace(job.key, Cached{std::move(pixels), ++tick, availability, GetTickCount64() + 2000, job.generation, directory_hint, inspected, measured});
            used += cost;
            ready(job.key);
        }
    }
};
ThumbnailCache::ThumbnailCache(fs::path profile) : impl_(std::make_shared<Impl>(std::move(profile))) {}
ThumbnailCache::~ThumbnailCache() { impl_->shutdown(); }
std::shared_ptr<const ImagePreview> ThumbnailCache::request(std::string hash, UINT format, unsigned width) {
    if (!cache::thumbnail_hash(hash))
        return {};
    // The same bytes could legally occur under two adapters.
    width = cache::thumbnail_width(width);
    const auto adapter = format == CF_DIB ? cache::dib : format == CF_DIBV5 ? cache::dibv5 : cache::png;
    hash = cache::thumbnail_key(hash, adapter, width);
    std::lock_guard guard(impl_->mutex);
    if (auto it = impl_->cached.find(hash); it != impl_->cached.end()) {
        it->second.last = ++impl_->tick;
        return it->second.image;
    }
    const auto op = format == CF_DIB ? ThumbnailOp::dib : format == CF_DIBV5 ? ThumbnailOp::dibv5 : ThumbnailOp::png;
    impl_->enqueue({hash, op, 0, width});
    return {};
}
domain::FilePreview ThumbnailCache::request_files(std::string hash, unsigned width, bool picture) {
    if (!cache::thumbnail_hash(hash)) return {};
    width = cache::thumbnail_width(width);
    std::lock_guard guard(impl_->mutex);
    hash = picture ? cache::thumbnail_key(hash, cache::files, width) : hash + "-status";
    const auto requested = impl_->refreshes.find(hash);
    const bool force = requested != impl_->refreshes.end();
    const bool explicit_read = force && GetTickCount64() <= requested->second.until;
    domain::FilePreview result;
    if (auto it = impl_->cached.find(hash); it != impl_->cached.end()) {
        auto& value = it->second;
        result = {value.image, value.availability, value.directory, value.source};
        value.last = ++impl_->tick;
        if (!force && value.generation == impl_->generation && GetTickCount64() < value.expires) return result;
    }
    if (const auto it = impl_->measuring.find(hash); it != impl_->measuring.end()) {
        if (result.image && result.source != it->second) result.image.reset();
        result.source = it->second;
    }
    impl_->enqueue({hash, picture ? ThumbnailOp::file_picture : ThumbnailOp::file_status, impl_->generation, width, explicit_read,
                    force ? requested->second.ticket : 0});
    return result;
}
void ThumbnailCache::recheck_files(std::string requested_list) {
    std::lock_guard guard(impl_->mutex);
    if (requested_list.empty()) {
        ++impl_->generation;
        impl_->measuring.clear();
        return;
    }
    const auto prefix = (requested_list + "-").append(cache::files).append("-");
    std::erase_if(impl_->measuring, [&](const auto& item) { return item.first.starts_with(prefix); });
    const auto ticket = ++impl_->next_refresh;
    // Include offscreen variants without retaining an unbounded list of old actions.
    if (impl_->refreshes.size() >= 512) {
        std::erase_if(impl_->refreshes, [&](const auto& entry) { return entry.second.ticket + 80 < ticket; });
    }
    const Impl::Refresh refresh{ticket, GetTickCount64() + 5000};
    impl_->refreshes[requested_list + "-status"] = refresh;
    for (unsigned width : cache::thumbnail_widths)
        impl_->refreshes[cache::thumbnail_key(requested_list, cache::files, width)] = refresh;
}
void ThumbnailCache::on_ready(std::function<void()> cb) {
    std::lock_guard guard(impl_->mutex);
    impl_->changes = {};
    impl_->notify = std::move(cb);
}
application::PreviewChanges ThumbnailCache::take_changes() {
    std::lock_guard guard(impl_->mutex);
    return std::exchange(impl_->changes, {});
}
std::size_t ThumbnailCache::bytes() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->used;
}
} // namespace cliprope::rendering
