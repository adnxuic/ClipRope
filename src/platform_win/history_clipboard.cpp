#include "platform_win/history_clipboard.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/process_image.hpp"
#include <algorithm>
#include <cstring>
#include "platform_win/screenshot_folder.hpp"

namespace cliprope::win {
namespace {
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto size = static_cast<int>(value.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    if (length) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value) {
    if (value.empty() || value.size() > 1024) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    if (length) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

// Out-of-context WinEvents and clipboard notifications run on the listener STA.
// Only a foreground snipping overlay can start a round. A second invocation in
// the same process still gets a fresh identity, even for identical pixels.
struct SystemSnip {
    HWND overlay{};
    DWORD pid{}, left_at{}, version{};
    unsigned publications{};
    std::uint64_t generation{};
};
thread_local SystemSnip snip;

void CALLBACK foreground_changed(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD tick) {
    if (window == snip.overlay) return;
    if (snip.overlay) {
        snip.overlay = nullptr;
        snip.left_at = tick;
    }
    wchar_t type[48]{};
    if (!window || !GetClassNameW(window, type, static_cast<int>(std::size(type))) ||
        wcscmp(type, L"XamlWindow") != 0) return;
    try {
        const DWORD pid = window_process(window);
        if (_wcsicmp(process_basename(pid).c_str(), L"SnippingTool.exe") != 0)
            return;
        FILETIME started{};
        GetSystemTimePreciseAsFileTime(&started);
        snip = {window, pid, 0, 0, 0, (std::uint64_t(started.dwHighDateTime) << 32) | started.dwLowDateTime};
    } catch (...) { snip.pid = 0; }
}

std::uint64_t snip_round(HWND owner, DWORD version) {
    const DWORD pid = window_process(owner);
    if (!snip.pid || pid != snip.pid || (!snip.overlay && GetTickCount() - snip.left_at > 3000)) {
        if (snip.publications) snip.pid = 0;
        return 0;
    }
    if (version != snip.version) {
        snip.version = version;
        ++snip.publications;
    }
    return snip.publications <= 2 ? snip.generation : 0;
}

} // namespace

domain::Snapshot capture_snapshot(CaptureResult result) {
    domain::Snapshot snapshot;
    snapshot.capture_status = std::move(result.outcome);
    snapshot.screenshot_path = utf8(result.screenshot_path);
    snapshot.path_kind = result.path_kind;
    snapshot.captured_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto& observed : result.formats) {
        domain::Format format;
        if (observed.format < 0xc000) format.standard_id = observed.format;
        else format.name = utf8(format_name(observed.format));
        format.tymed = observed.tymed;
        format.aspect = observed.aspect;
        format.index = observed.index;
        format.priority = static_cast<int>(snapshot.formats.size());
        if (observed.outcome == "saved") format.outcome = "saved";
        else if (observed.outcome.starts_with("unsupported-") || observed.outcome == "duplicate") format.outcome = "skipped";
        else format.outcome = "failed";
        if (format.outcome == "saved") {
            const auto found = std::find_if(result.saved.begin(), result.saved.end(),
                [&](const auto& item) { return item.format == observed.format && item.index == observed.index &&
                    item.aspect == observed.aspect && (item.tymed & observed.tymed) != 0; });
            if (found == result.saved.end()) throw std::runtime_error("Missing captured bytes");
            format.tymed = found->tymed;
            format.codec_version = found->codec_version;
            format.bytes = std::move(found->bytes);
        }
        snapshot.formats.push_back(std::move(format));
    }
    return snapshot;
}

static PastePayload file_payload(const domain::Snapshot& snapshot) {
    for (const auto& f : snapshot.formats) {
        if (f.standard_id == CF_HDROP && f.outcome == "saved")
            return image_file_group(file_group_paths(f.bytes)) ? PastePayload::image_files : PastePayload::files;
    }
    return PastePayload::other;
}
static std::vector<Representation> decode_manifest(const domain::Snapshot& snapshot, bool files_only) {
    std::vector<Representation> saved;
    for (const auto& format : snapshot.formats) {
        if (format.outcome != "saved") continue;
        UINT id = format.standard_id;
        if (!id) {
            auto name = wide(format.name);
            if (name.empty()) return {};
            id = RegisterClipboardFormatW(name.c_str());
        }
        if (files_only && !is_file_group_format(id)) continue;
        saved.push_back({id, format.bytes, format.index, format.tymed, format.aspect, format.codec_version});
    }
    return saved;
}

std::vector<Representation> restorable_formats(const domain::Snapshot& snapshot) {
    auto result = decode_manifest(snapshot, file_payload(snapshot) == PastePayload::image_files);
    if (FAILED(prepare_publication(result))) result.clear();
    return result;
}

application::PublicationId HistoryClipboard::publish(const domain::LeasedSnapshot& content, bool for_folder,
                                                     std::optional<DWORD> expected_sequence) {
    const auto sequence = GetClipboardSequenceNumber();
    auto offer = prepare(content.snapshot, for_folder);
    if (!offer) return 0;
    if (offer->screenshot_file && !expected_sequence) expected_sequence = sequence;
    return publish_prepared(std::move(*offer), content.lease, expected_sequence);
}

std::optional<HistoryClipboard::Offer> HistoryClipboard::prepare(const domain::Snapshot& snapshot, bool for_folder) {
    const auto files = file_payload(snapshot);
    auto saved = decode_manifest(snapshot, files == PastePayload::image_files ||
                                                   (for_folder && files == PastePayload::files));
    const auto png = RegisterClipboardFormatW(L"PNG");
    const auto html = RegisterClipboardFormatW(L"HTML Format");
    const auto rtf = RegisterClipboardFormatW(L"Rich Text Format");
    const bool text_only = !saved.empty() && std::all_of(saved.begin(), saved.end(), [html, rtf](const auto& f) {
        return f.format == CF_UNICODETEXT || f.format == html || f.format == rtf;
    });
    const bool screenshot = for_folder && !saved.empty() && std::all_of(saved.begin(), saved.end(), [png](const auto& f) {
        return f.format == png || f.format == CF_DIBV5 || f.format == CF_DIB;
    });
    Offer offer;
    if (FAILED(ClipboardOffer::prepare(std::move(saved), screenshot, offer.formats))) return {};
    offer.payload = screenshot ? PastePayload::files : text_only ? PastePayload::text : files;
    offer.screenshot_file = screenshot;
    return offer;
}

application::PublicationId HistoryClipboard::publish_prepared(Offer offer, std::shared_ptr<void> lease,
                                                             std::optional<DWORD> expected) {
    if (FAILED(publisher_.publish_prepared(std::move(offer.formats), std::move(lease), expected))) return 0;
    payload_ = offer.payload;
    return ++generation_;
}

bool HistoryClipboard::owns_publication(application::PublicationId receipt) const {
    // OLE can dispatch a nested publication while answering the identity query.
    return receipt && receipt == generation_ && publisher_.has_settled_publication() && receipt == generation_;
}

namespace {
application::Delivery map_delivery(DeliveryResult result) {
    using application::Delivery;
    switch (result) {
    case DeliveryResult::shortcut_sent: return Delivery::shortcut_sent;
    case DeliveryResult::send_failed: return Delivery::send_failed;
    case DeliveryResult::no_field: return Delivery::no_field;
    case DeliveryResult::lost_target: return Delivery::lost_target;
    case DeliveryResult::access_denied: return Delivery::denied;
    case DeliveryResult::replaced_publication: return Delivery::replaced;
    default: return Delivery::unconfirmed;
    }
}

} // namespace
application::Delivery HistoryClipboard::deliver(EditGesture& gesture, application::PublicationId receipt) {
    return map_delivery(gesture.deliver([this, receipt] { return owns_publication(receipt); }, payload_));
}
std::optional<application::Delivery> HistoryClipboard::poll_delivery(EditGesture& gesture, application::PublicationId receipt) {
    auto result = gesture.poll_delivery([this, receipt] { return owns_publication(receipt); }, payload_);
    if (!result) return {};
    return map_delivery(*result);
}

void HistoryCapture::watch_system_snips() {
    if (!snip_hook_) snip_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, foreground_changed, 0, 0, WINEVENT_OUTOFCONTEXT);
}

HistoryCapture::HistoryCapture(application::History& store, HistoryClipboard& clipboard,
                               decltype(&CreateThreadpoolWait) create_wait, application::Admission::Write write)
    : admissions_([&store, write = std::move(write)](const application::Admission& item) {
          return write ? item.commit(write) : item.commit(store);
      }, [this](auto serial) { if (wake) wake(serial); }),
      coordinator_(store, [this](application::Admission item) { return admissions_.submit(std::move(item)); }),
      clipboard_(clipboard), create_wait_(create_wait) {}

HistoryCapture::~HistoryCapture() {
    abandon();
    if (snip_hook_) UnhookWinEvent(snip_hook_);
}

bool HistoryCapture::settle_completed_snips() {
    bool saved = stop_observation();
    admissions_.drain_writes();
    while (auto result = admissions_.next())
        if (result->outcome.status == "history-failed") saved = false;
    accepted_.clear();
    return saved;
}

bool HistoryCapture::stop_observation() {
    bool saved = true;
    while (auto outcome = coordinator_.settle(true))
        if (outcome->status == "history-failed") saved = false;
    abandon(); // Accepted writes remain owned until the STA receives their terminal result.
    return saved;
}

application::CaptureOutcome HistoryCapture::observe(const application::CapturePolicy& policy) {
    return observe_bound(notification(), policy);
}
HistoryCapture::Observation HistoryCapture::notification() {
    const auto at = std::chrono::steady_clock::now();
    const auto version = GetClipboardSequenceNumber();
    const bool self = clipboard_.owns_current();
    clipboard_.release_if_replaced();
    const auto owner = GetClipboardOwner();
    return {version, owner, self, utf8(process_basename(window_process(owner))),
            snip_hook_ ? snip_round(owner, version) : 0, at};
}
application::CaptureOutcome HistoryCapture::observe_bound(Observation observation, const application::CapturePolicy& policy) {
    const auto version = observation.version;
    if (version != GetClipboardSequenceNumber() || observation.owner != GetClipboardOwner()) return {"superseded", {}};
    auto source = std::move(observation.source);
    if (!policy.excluded_apps.empty() && !source.empty()) {
        const auto basename = wide(source);
        for (const auto& app : policy.excluded_apps) {
            const auto excluded = wide(app);
            if (!excluded.empty() && CompareStringOrdinal(basename.c_str(), -1, excluded.c_str(), -1, TRUE) == CSTR_EQUAL) {
                source = app;
                break;
            }
        }
    }
    application::CaptureOutcome outcome;
    auto ticket = coordinator_.begin(version, std::move(source), observation.self, policy,
                                     outcome, observation.snip_round, observation.at);
    health_.observed_sequence = version;
    health_.observation = state_code(capture_states, outcome.status);
    if (outcome.status != "duplicate") waiting_ = std::move(ticket);
    dispatch();
    return outcome;
}

void HistoryCapture::dispatch() {
    if (!waiting_) return;
    for (auto& slot : slots_) {
        if (slot.ticket) continue;
        slot.ticket = std::move(waiting_);
        waiting_.reset();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            slot.ticket->expires - std::chrono::steady_clock::now()).count();
        const auto deadline_tick = GetTickCount64() + std::max<std::int64_t>(0, remaining);
        try { slot.process = std::make_unique<worker::Process>(slot.ticket->serial, slot.ticket->version, deadline_tick,
                                                               wake, create_wait_); }
        catch (const std::system_error& error) { slot.startup_error = error.code().value(); }
        catch (const std::exception&) { slot.startup_error = ERROR_NOT_ENOUGH_MEMORY; }
        if (!slot.process && wake) wake(slot.ticket->serial);
        break;
    }
}

std::vector<application::CaptureOutcome> HistoryCapture::poll() {
    std::vector<application::CaptureOutcome> out;
    for (auto& slot : slots_) {
        if (!slot.ticket) continue;
        const bool expired = std::chrono::steady_clock::now() >= slot.ticket->expires ||
                             (slot.process && slot.process->timed_out());
        if (!expired && slot.process && !slot.process->ready()) continue;
        CaptureHealth::Completion receipt;
        receipt.sequence = slot.ticket->version;
        domain::Snapshot snapshot;
        std::string detail;
        // A completed obsolete response can contain tens of MiB. Reject its
        // binding before mapping or allocating any payload in the desktop.
        if (!coordinator_.current(*slot.ticket, GetClipboardSequenceNumber())) snapshot.capture_status = "superseded";
        else if (expired) {
            snapshot.capture_status = "expired";
            if (slot.process && slot.process->termination_error())
                detail = "job-termination-win32=" + std::to_string(slot.process->termination_error());
        }
        else if (!slot.process) {
            snapshot.capture_status = "worker-unavailable";
            detail = "win32=" + std::to_string(slot.startup_error);
        } else try {
            auto result = slot.process->take(slot.ticket->serial, slot.ticket->version, receipt.worker_exit);
            const auto text = std::find_if(result.formats.begin(), result.formats.end(),
                [](const auto& row) { return row.format == CF_UNICODETEXT; });
            receipt.unicode = state_code(unicode_states, text == result.formats.end() ? "absent" : text->outcome);
            receipt.hresult = static_cast<DWORD>(result.error);
            receipt.oom = result.oom;
            if (result.oom.guard != NativeGuard::none) receipt.job_peak_kib = slot.process->peak_commit_kib();
            receipt.format_count = static_cast<DWORD>(result.formats.size());
            receipt.saved_count = static_cast<DWORD>(result.saved.size());
            receipt.retries = result.retries;
            detail = "formats=" + std::to_string(result.formats.size()) + " saved=" + std::to_string(result.saved.size()) +
                     " retries=" + std::to_string(result.retries) + " hr=" + std::to_string(static_cast<unsigned long>(result.error)) +
                     " unicode=" + (text == result.formats.end() ? "absent" : text->outcome) +
                     " exit=" + std::to_string(receipt.worker_exit);
            if (receipt.job_peak_kib) detail += " job-peak-kib=" + std::to_string(receipt.job_peak_kib);
            if (receipt.oom.guard != NativeGuard::none)
                detail += " native-guard=" + std::string(recovery_guard_name(static_cast<DWORD>(receipt.oom.guard))) +
                          " reacquisitions=" + std::to_string(receipt.oom.reacquisitions) +
                          " monitor-hr=" + std::to_string(static_cast<DWORD>(receipt.oom.monitor_query)) +
                          " history-hr=" + std::to_string(static_cast<DWORD>(receipt.oom.history_query)) +
                          " native-first=" + std::to_string(receipt.oom.first_format) +
                          " native-count=" + std::to_string(receipt.oom.native_count) +
                          " native-error=" + std::to_string(receipt.oom.native_error);
            snapshot = capture_snapshot(std::move(result));
            const bool tracked = slot.ticket->snip_round && snip.generation == slot.ticket->snip_round;
            if (tracked || system_snip_source(slot.ticket->source))
                snapshot.snip_directory = utf8(native_screenshot_folder());
            if (tracked) {
                FILETIME until{};
                GetSystemTimePreciseAsFileTime(&until);
                snapshot.snip_begin = slot.ticket->snip_round;
                snapshot.snip_end = (std::int64_t(until.dwHighDateTime) << 32) | until.dwLowDateTime;
            }
        }
        catch (const std::exception&) { snapshot.capture_status = "worker-invalid"; }
        auto ticket = std::move(*slot.ticket);
        slot = {}; // Closing the job also terminates a permanently blocked COM client.
        auto outcome = coordinator_.finish(ticket, GetClipboardSequenceNumber(), std::move(snapshot));
        detail = "seq=" + std::to_string(ticket.version) + " " + detail;
        if (outcome.admission_serial) {
            accepted_.emplace(outcome.admission_serial, AcceptedReceipt{receipt, std::move(detail)});
        } else {
            receipt.outcome = state_code(capture_states, outcome.status);
            health_.completion = receipt;
            outcome.detail = detail + (outcome.detail.empty() ? "" : " " + outcome.detail);
            out.push_back(std::move(outcome));
        }
    }
    if (waiting_ && std::chrono::steady_clock::now() >= waiting_->expires) {
        health_.completion = {waiting_->version, state_code(capture_states, "expired")};
        auto expired = std::move(*waiting_);
        waiting_.reset();
        domain::Snapshot failed;
        failed.capture_status = "expired";
        coordinator_.finish(expired, GetClipboardSequenceNumber(), std::move(failed));
        out.push_back({"expired", {}});
    }
    dispatch();
    while (auto settled = coordinator_.settle()) {
        if (!settled->admission_serial) out.push_back(std::move(*settled));
    }
    while (auto result = admissions_.next()) {
        auto found = accepted_.extract(result->serial);
        auto receipt = found.empty() ? CaptureHealth::Completion{result->sequence} : found.mapped().receipt;
        receipt.outcome = state_code(capture_states, result->outcome.status);
        health_.completion = receipt;
        result->outcome.detail = found.empty() ? "seq=" + std::to_string(result->sequence) : std::move(found.mapped().detail);
        out.push_back(std::move(result->outcome));
    }
    return out;
}

bool HistoryCapture::busy() const {
    return writing() || coordinator_.settling() || waiting_.has_value() || std::any_of(slots_.begin(), slots_.end(),
                                               [](const auto& slot) { return slot.ticket.has_value(); });
}

void HistoryCapture::abandon() {
    coordinator_.abandon();
    waiting_.reset();
    for (auto& slot : slots_) slot = {};
}
} // namespace cliprope::win
