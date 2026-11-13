#pragma once
#include "persistence/card_search.hpp"
#include <condition_variable>
#include <thread>

namespace cliprope {
// The reader, ordered counts and text buffers live on this single worker.
// The newest generation owns the only completion slot.
class FindWorker final {
public:
    struct Request {
        application::FindScope scope;
        std::wstring term;
        size_t index{};
        std::uint64_t version{};
        std::optional<application::SearchHit> retain;
    };
    enum class Error { none, unavailable, unsupported_format };
    struct Reply {
        size_t index{}, total{};
        std::optional<application::SearchHit> hit;
        Error error{};
    };
    FindWorker(std::filesystem::path root, std::function<void()> notify);
    ~FindWorker();
    void submit(Request request);
    void cancel();
    std::optional<Reply> take();

private:
    void run(std::stop_token stop);
    std::filesystem::path root_;
    std::function<void()> notify_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::stop_source current_;
    std::optional<Request> pending_;
    std::optional<Reply> reply_;
    std::jthread worker_;
};
} // namespace cliprope
