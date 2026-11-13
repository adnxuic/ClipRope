#pragma once

#include "application/capture.hpp"
#include <windows.h>
#include <array>
#include <mutex>

namespace cliprope::win {
// One writer, two owned slots (including undelivered receipts). A snip settlement
// and the ordinary capture which triggered it can both submit in one STA turn.
class CaptureAdmissions {
public:
    using Write = std::function<application::CaptureOutcome(const application::Admission&)>;
    using Wake = std::function<void(std::uint64_t)>;
    CaptureAdmissions(Write write, Wake wake);
    ~CaptureAdmissions();
    application::CaptureOutcome submit(application::Admission);
    struct Result {
        std::uint64_t serial{};
        std::uint32_t sequence{};
        application::CaptureOutcome outcome;
    };
    std::optional<Result> next();
    void drain_writes();
    bool busy() const;
    size_t peak_bytes() const;

private:
    struct Slot {
        std::optional<application::Admission> pending;
        std::optional<Result> result;
    };
    static void CALLBACK run(PTP_CALLBACK_INSTANCE, void*, PTP_WORK);
    void write_batch();
    Write write_;
    Wake wake_;
    PTP_WORK work_{};
    mutable std::mutex mutex_;
    std::array<Slot,2> slots_;
    size_t first_{}, count_{}, bytes_{}, peak_{};
    bool running_{};
};
}
