#include "platform_win/capture_admissions.hpp"
#include "platform_win/clipboard.hpp"
#include <algorithm>
#include <utility>

namespace cliprope::win {
namespace {
constexpr size_t max_bytes = snapshot_byte_limit;
size_t payload_bytes(const domain::Snapshot& snapshot) {
    size_t size{};
    for (const auto& format : snapshot.formats) {
        if (format.bytes.size() > max_bytes - size) return max_bytes + 1;
        size += format.bytes.size();
    }
    return size;
}
}
CaptureAdmissions::CaptureAdmissions(Write write, Wake wake) : write_(std::move(write)), wake_(std::move(wake)) {}
CaptureAdmissions::~CaptureAdmissions() {
    drain_writes();
    if (work_) CloseThreadpoolWork(work_);
}

application::CaptureOutcome CaptureAdmissions::submit(application::Admission accepted) {
    const auto size = payload_bytes(accepted.snapshot);
    std::lock_guard lock(mutex_);
    if (count_ == slots_.size() || size > max_bytes - bytes_ || accepted.snapshot.formats.size() > 256)
        return {"history-failed", {}, "admission capacity"};
    if (!work_) work_ = CreateThreadpoolWork(run, this, nullptr);
    if (!work_) return {"history-failed", {}, "writer unavailable"};
    const auto serial = accepted.ticket.serial;
    slots_[(first_ + count_) % slots_.size()].pending.emplace(std::move(accepted));
    ++count_;
    bytes_ += size;
    peak_ = std::max(peak_, bytes_);
    if (!running_) {
        running_ = true;
        SubmitThreadpoolWork(work_);
    }
    return {"admission-pending", {}, {}, false, serial};
}

void CALLBACK CaptureAdmissions::run(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
    static_cast<CaptureAdmissions*>(context)->write_batch();
}
void CaptureAdmissions::write_batch() {
    for (;;) {
        size_t index{};
        std::optional<application::Admission> job;
        {
            std::lock_guard lock(mutex_);
            for (size_t n = 0; n < count_; ++n) {
                index = (first_ + n) % slots_.size();
                if (slots_[index].pending) {
                    job = std::exchange(slots_[index].pending, {});
                    break;
                }
            }
            if (!job) { running_ = false; return; }
        }
        const auto size = payload_bytes(job->snapshot);
        const auto serial = job->ticket.serial;
        Result result{serial, job->ticket.version};
        try { result.outcome = write_(*job); }
        catch (const std::exception&) { result.outcome = {"history-failed", {}}; }
        job.reset(); // Release buffers before releasing their budget.
        {
            std::lock_guard lock(mutex_);
            slots_[index].result.emplace(std::move(result));
            bytes_ -= size;
        }
        if (wake_) wake_(serial);
    }
}
std::optional<CaptureAdmissions::Result> CaptureAdmissions::next() {
    std::lock_guard lock(mutex_);
    if (!count_ || !slots_[first_].result) return {};
    auto result = std::exchange(slots_[first_].result, {});
    first_ = (first_ + 1) % slots_.size();
    --count_;
    return result;
}
size_t CaptureAdmissions::peak_bytes() const {
    std::lock_guard lock(mutex_);
    return peak_;
}
bool CaptureAdmissions::busy() const {
    std::lock_guard lock(mutex_);
    return count_ != 0;
}
void CaptureAdmissions::drain_writes() {
    if (work_) WaitForThreadpoolWorkCallbacks(work_, FALSE);
}
}
