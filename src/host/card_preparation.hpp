#pragma once
#include "application/history.hpp"
#include "platform_win/history_clipboard.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <thread>

namespace cliprope::host {
// The worker owns file and snapshot data. The UI retains the completion,
// including any window or OLE gesture, until the result has been joined.
class CardPreparation {
public:
    static constexpr UINT ready_message = WM_APP + 0x570;
    struct Result {
        application::PreparedSnapshot snapshot;
        std::optional<win::HistoryClipboard::Offer> offer;
        std::filesystem::path path;
        bool folder_only{};
    };
    using Work = std::function<Result(std::stop_token)>;
    using Completed = std::function<void(std::future<Result>)>;

    bool active() const { return result_.valid(); }
    void start(HWND listener, Work work, Completed completed, HWND source = nullptr) {
        if (active()) throw std::logic_error("A card action is already being prepared");
        std::packaged_task<Result(std::stop_token)> task(std::move(work));
        auto future = task.get_future();
        if (!SetTimer(listener, ready_message, 32, nullptr)) throw std::runtime_error("Card preparation timer unavailable");
        try {
            worker_ = std::jthread([listener, task = std::move(task)](std::stop_token stop) mutable {
                task(stop);
                PostMessageW(listener, ready_message, 0, 0);
            });
        } catch (...) {
            KillTimer(listener, ready_message);
            throw;
        }
        listener_ = listener;
        source_ = source;
        result_ = std::move(future);
        completed_ = std::move(completed);
    }
    void cancel() {
        if (worker_.joinable()) {
            worker_.request_stop();
            CancelSynchronousIo(worker_.native_handle());
        }
    }
    void cancel_source(HWND source) {
        if (source && source_ == source) cancel();
    }
    bool poll() {
        if (!active() || result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        const bool cancelled = worker_.get_stop_token().stop_requested();
        worker_.join();
        KillTimer(listener_, ready_message);
        source_ = nullptr;
        auto ready = std::move(result_);
        auto complete = std::move(completed_);
        if (!cancelled) complete(std::move(ready));
        return true;
    }
    void shutdown() {
        cancel();
        if (worker_.joinable()) worker_.join();
        if (listener_) KillTimer(listener_, ready_message);
        result_ = {};
        completed_ = {};
    }
private:
    HWND listener_{}, source_{};
    std::future<Result> result_;
    Completed completed_;
    std::jthread worker_;
};
}
