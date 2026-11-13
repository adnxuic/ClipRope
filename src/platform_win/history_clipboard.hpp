#pragma once

#include "application/capture.hpp"
#include "application/invoke.hpp"
#include "capture_worker/process.hpp"
#include "platform_win/delivery.hpp"
#include "platform_win/performance_counters.hpp"
#include "platform_win/capture_admissions.hpp"
#include <array>
#include <map>

namespace cliprope::win {

domain::Snapshot capture_snapshot(CaptureResult result);
std::vector<Representation> restorable_formats(const domain::Snapshot& snapshot);

class HistoryClipboard {
public:
    struct Offer {
        ClipboardOffer formats;
        PastePayload payload = PastePayload::other;
        bool screenshot_file{};
    };
    static std::optional<Offer> prepare(const domain::Snapshot& snapshot, bool for_folder);
    application::PublicationId publish_prepared(Offer offer, std::shared_ptr<void> lease, std::optional<DWORD> expected);
    application::PublicationId publish(const domain::LeasedSnapshot& content, bool for_folder = false,
                                       std::optional<DWORD> expected_sequence = {});
    application::Delivery deliver(EditGesture& gesture, application::PublicationId receipt);
    std::optional<application::Delivery> poll_delivery(EditGesture&, application::PublicationId);
    bool owns_publication(application::PublicationId receipt) const;
    bool owns_current() const { return publisher_.is_current(); }
    void release_if_replaced() { publisher_.release_if_replaced(); }
    HRESULT flush_if_current() { return publisher_.flush_if_current(); }
    FlushReceipt flush_current() { return publisher_.flush_current(); }
private:
    ClipboardPublisher publisher_;
    PastePayload payload_ = PastePayload::other;
    application::PublicationId generation_{};
};

// UI-STA facade. observe binds the target and dispatches; poll drains completed
// tickets without waiting for a foreign IDataObject. Call poll from the host timer.
class HistoryCapture {
public:
    HistoryCapture(application::History& store, HistoryClipboard& clipboard,
                   decltype(&CreateThreadpoolWait) create_wait = CreateThreadpoolWait,
                   application::Admission::Write write = {});
    // Set before observation. The callback only posts a content-free hint.
    worker::Process::Wake wake;
    struct Observation {
        DWORD version;
        HWND owner;
        bool self;
        std::string source;
        std::uint64_t snip_round;
        std::chrono::steady_clock::time_point at;
    };
    Observation notification();
    application::CaptureOutcome observe_bound(Observation observation, const application::CapturePolicy& policy);
    application::CaptureOutcome observe(const application::CapturePolicy& policy = {});
    ~HistoryCapture();
    void watch_system_snips();
    bool settle_completed_snips();
    bool stop_observation();
    std::vector<application::CaptureOutcome> poll();
    void abandon();
    bool busy() const;
    bool writing() const { return admissions_.busy(); }
    size_t peak_pending_bytes() const { return admissions_.peak_bytes(); }
    const CaptureHealth& health() const { return health_; }
private:
    struct Slot {
        std::unique_ptr<worker::Process> process;
        std::optional<application::CaptureTicket> ticket;
        DWORD startup_error{};
    };
    void dispatch();
    CaptureAdmissions admissions_;
    struct AcceptedReceipt {
        CaptureHealth::Completion receipt;
        std::string detail;
    };
    std::map<std::uint64_t, AcceptedReceipt> accepted_;
    application::CaptureCoordinator coordinator_;
    HistoryClipboard& clipboard_;
    decltype(&CreateThreadpoolWait) create_wait_;
    std::array<Slot, 2> slots_;
    std::optional<application::CaptureTicket> waiting_;
    CaptureHealth health_;
    HWINEVENTHOOK snip_hook_{};
};

} // namespace cliprope::win
