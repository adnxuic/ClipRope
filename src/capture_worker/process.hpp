#pragma once

#include "capture_worker/protocol.hpp"
#include <functional>
#include <atomic>
#include <mutex>

namespace cliprope::worker {
class Process {
public:
    using Wake = std::function<void(std::uint64_t)>;
    Process(std::uint64_t serial, DWORD sequence, ULONGLONG deadline_tick, Wake wake = {},
            decltype(&CreateThreadpoolWait) create_wait = CreateThreadpoolWait);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    bool ready() const;
    DWORD peak_commit_kib() const;
    DWORD termination_error() const;
    bool timed_out() const { return timed_out_.load(); }
    CaptureResult take(std::uint64_t serial, DWORD sequence, DWORD& exit_code) const;
private:
    HANDLE job_{}, section_{}, process_{};
    PTP_WAIT wait_{};
    const std::uint64_t serial_;
    Wake wake_;
    mutable std::mutex job_mutex_;
    DWORD termination_error_{};
    std::atomic_bool timed_out_{};
    static void CALLBACK completed(PTP_CALLBACK_INSTANCE, void*, PTP_WAIT, TP_WAIT_RESULT);
    void close();
};
} // namespace cliprope::worker
