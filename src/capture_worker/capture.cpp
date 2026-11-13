#include "capture_worker/capture.hpp"
#include "capture_worker/file_group_capture.hpp"
#include "platform_win/file_group.hpp"
#include "capture_worker/stream_reader.hpp"
#include "capture_worker/virtual_streams.hpp"
#include "platform_win/virtual_files.hpp"
#include "platform_win/process_image.hpp"
#include "platform_win/image_file_access.hpp"
#include <algorithm>
#include <deque>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace cliprope {
namespace {

template<class T> struct ComRelease { void operator()(T* object) const { object->Release(); } };
struct Medium {
    STGMEDIUM value{};
    ~Medium() { ReleaseStgMedium(&value); }
};
struct GlobalView {
    HGLOBAL memory;
    const unsigned char* data;
    explicit GlobalView(HGLOBAL handle) : memory(handle), data(static_cast<const unsigned char*>(GlobalLock(handle))) {}
    ~GlobalView() { if (data) GlobalUnlock(memory); }
};

bool contention(HRESULT hr) {
    return hr == CLIPBRD_E_CANT_OPEN || hr == HRESULT_FROM_WIN32(ERROR_BUSY) ||
           hr == RPC_E_SERVERCALL_RETRYLATER;
}

std::string save_global(HGLOBAL memory, UINT format, std::size_t remaining, CaptureResult& result) {
    if (!memory) return "invalid-medium";
    const auto size = GlobalSize(memory);
    if (!size || size > representation_byte_limit(format) || size > remaining) return "size-rejected";
    GlobalView view(memory);
    if (!view.data) return "lock-failed";
    Representation item{format, {view.data, view.data + size}};
    if (!validate_representation(item)) return "invalid-content";
    if (item.bytes.size() > remaining) return "size-rejected";
    result.saved.push_back(std::move(item));
    return "saved";
}

// Windows can expose CF_DIB for an offered CF_BITMAP without listing it in
// the OLE inventory. Preserve that native representation alongside the PNG.
void save_snip_dib(CaptureResult& result, HWND owner, const std::function<bool()>& changed) {
    const auto png = RegisterClipboardFormatW(L"PNG");
    if (result.formats.size() >= 256 ||
        std::none_of(result.formats.begin(), result.formats.end(), [](const auto& f) {
            return f.format == CF_BITMAP && (f.tymed & TYMED_GDI) && f.aspect == DVASPECT_CONTENT && f.index == -1;
        }) ||
        std::none_of(result.saved.begin(), result.saved.end(), [png](const auto& f) { return f.format == png; }) ||
        std::any_of(result.formats.begin(), result.formats.end(), [](const auto& f) { return f.format == CF_DIB; }) ||
        _wcsicmp(win::process_basename(win::window_process(owner)).c_str(), L"SnippingTool.exe"))
        return;
    if (!OpenClipboard(nullptr)) return;
    struct Close { ~Close() { CloseClipboard(); } } close;
    if (changed() || !IsClipboardFormatAvailable(CF_DIB)) return;
    std::size_t remaining = snapshot_byte_limit;
    for (const auto& item : result.saved) remaining -= item.bytes.size();
    auto outcome = save_global(GetClipboardData(CF_DIB), CF_DIB, remaining, result);
    result.formats.push_back({CF_DIB, TYMED_HGLOBAL, DVASPECT_CONTENT, -1, std::move(outcome)});
}

std::string extract(IDataObject* source, FORMATETC format, CaptureResult& result, CaptureMode mode,
                    const std::function<bool()>& superseded) {
    if (mode != CaptureMode::format_probe && format.cfFormat == virtual_files::contents_format())
        return "indexed-offer";
    const bool supported = mode == CaptureMode::format_probe ?
        format.cfFormat == CF_UNICODETEXT || format.cfFormat == RegisterClipboardFormatW(L"HTML Format") :
        supports_hglobal(format.cfFormat);
    if (!supported)
        return "unsupported-format";
    const bool stream_png = format.cfFormat == RegisterClipboardFormatW(L"PNG") && (format.tymed & TYMED_ISTREAM);
    if ((!(format.tymed & TYMED_HGLOBAL) && !stream_png) || format.dwAspect != DVASPECT_CONTENT ||
        format.lindex != -1 || format.ptd) return "unsupported-medium";
    for (const auto& item : result.saved) if (item.format == format.cfFormat) return "duplicate";
    std::size_t total{};
    for (const auto& item : result.saved) total += item.bytes.size();
    if (stream_png) {
        Representation image{format.cfFormat, {}, -1, TYMED_ISTREAM};
        const auto limit = std::min(representation_byte_limit(image.format), snapshot_byte_limit - total);
        auto status = read_bounded_stream(*source, image, limit, {}, result.error, superseded);
        if (status != "saved") return status;
        if (!validate_representation(image)) return "invalid-content";
        result.saved.push_back(std::move(image));
        return "saved";
    }
    format.tymed = TYMED_HGLOBAL;
    Medium medium;
    if (auto hr = source->GetData(&format, &medium.value); FAILED(hr)) {
        result.error = hr;
        return contention(hr) ? "clipboard-busy" : "get-data-failed";
    }
    if (medium.value.tymed != TYMED_HGLOBAL) return "invalid-medium";
    return save_global(medium.value.hGlobal, format.cfFormat, snapshot_byte_limit - total, result);
}

enum class Eligibility { allow, exclude, busy, unknown };
bool absent_clipboard_format(HRESULT result) {
    // Windows' IDataObject wrapper for a native clipboard returns
    // DV_E_CLIPFORMAT for an absent marker. OLE producers also use DV_E_FORMATETC.
    return result == DV_E_CLIPFORMAT || result == DV_E_FORMATETC;
}
Eligibility local_history_eligibility(IDataObject* source, OomEvidence& evidence) {
    const auto marker = [](const wchar_t* name) {
        return FORMATETC{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    };
    auto exclude = marker(L"ExcludeClipboardContentFromMonitorProcessing");
    auto include = marker(L"CanIncludeInClipboardHistory");
    if (!exclude.cfFormat || !include.cfFormat || IsClipboardFormatAvailable(exclude.cfFormat))
        return Eligibility::exclude;
    auto offered = source->QueryGetData(&exclude);
    evidence.monitor_query = offered;
    if (offered == S_OK) return Eligibility::exclude;
    if (contention(offered)) return Eligibility::busy;
    if (!absent_clipboard_format(offered)) return Eligibility::unknown;
    if (!IsClipboardFormatAvailable(include.cfFormat)) {
        offered = source->QueryGetData(&include);
        evidence.history_query = offered;
        if (contention(offered)) return Eligibility::busy;
        if (offered != S_OK) return absent_clipboard_format(offered) ? Eligibility::allow : Eligibility::unknown;
    }
    Medium flag;
    const auto hr = source->GetData(&include, &flag.value);
    evidence.history_query = hr;
    if (contention(hr)) return Eligibility::busy;
    if (FAILED(hr) || flag.value.tymed != TYMED_HGLOBAL || !flag.value.hGlobal ||
        GlobalSize(flag.value.hGlobal) < sizeof(DWORD)) return Eligibility::exclude;
    GlobalView bytes(flag.value.hGlobal);
    DWORD value{};
    if (bytes.data) std::memcpy(&value, bytes.data, sizeof(value));
    return value == 1 ? Eligibility::allow : Eligibility::exclude;
}

CaptureResult recover_native_unicode(DWORD sequence, HWND owner, OomEvidence& evidence) {
    CaptureResult failed{{}, {}, "enumeration-failed", E_OUTOFMEMORY};
    if (!owner) { evidence.guard = NativeGuard::no_owner; return failed; }
    if (!OpenClipboard(nullptr)) {
        evidence.native_error = GetLastError();
        evidence.guard = NativeGuard::open_failed;
        return failed;
    }
    struct Unlock { ~Unlock() { CloseClipboard(); } } unlock;
    auto current = [&] { return GetClipboardOwner() == owner && GetClipboardSequenceNumber() == sequence; };
    evidence.guard = NativeGuard::changed;
    if (!current()) return {{}, {}, "superseded"};
    // Raw Unicode is enumerated before its synthesized text companions. Any
    // other format, including an OLE envelope or privacy marker, disqualifies
    // this narrow recovery before a payload is requested.
    CaptureResult result{{}, {}, "partial", E_OUTOFMEMORY};
    UINT id{};
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        id = EnumClipboardFormats(id);
        if (!id) {
            evidence.native_error = GetLastError();
            if (evidence.native_error != ERROR_SUCCESS) { evidence.guard = NativeGuard::enum_failed; return failed; }
            break;
        }
        ++evidence.native_count;
        if (result.formats.empty()) {
            evidence.first_format = std::min(id, 0xc000u); // Group all registered IDs together.
            if (id != CF_UNICODETEXT) { evidence.guard = NativeGuard::first_not_unicode; return failed; }
        }
        if (result.formats.size() >= 4) { evidence.guard = NativeGuard::too_many; return failed; }
        if (id != CF_UNICODETEXT && id != CF_TEXT && id != CF_OEMTEXT && id != CF_LOCALE) {
            evidence.guard = NativeGuard::mixed;
            return failed;
        }
        result.formats.push_back({id, TYMED_HGLOBAL, DVASPECT_CONTENT, -1, "unsupported-format"});
    }
    if (result.formats.empty()) { evidence.guard = NativeGuard::empty; return failed; }
    result.formats.front().outcome = save_global(GetClipboardData(CF_UNICODETEXT), CF_UNICODETEXT, snapshot_byte_limit, result);
    if (!current()) return {{}, {}, "superseded"};
    if (result.saved.empty()) result.outcome = "no-usable-format";
    evidence.guard = result.saved.empty() ? NativeGuard::invalid_unicode : NativeGuard::recovered;
    return result;
}

CaptureResult capture_attempt(DWORD sequence, HWND owner, CaptureMode mode, bool& confirmed_contention,
                              const AcquireClipboard& acquire, bool& acquired) {
    if (!sequence || sequence != GetClipboardSequenceNumber() || GetClipboardOwner() != owner)
        return {{}, {}, "superseded"};
    // Some OLE proxies expose an empty enumerator while a clipboard reader
    // has the station locked. Detect that condition without touching payloads.
    if (!OpenClipboard(nullptr)) {
        const auto hr = HRESULT_FROM_WIN32(GetLastError());
        confirmed_contention = GetOpenClipboardWindow() != nullptr || contention(hr);
        return {{}, {}, "clipboard-busy", hr};
    }
    CloseClipboard();
    IDataObject* raw_source{};
    acquired = true;
    auto hr = acquire(&raw_source);
    if (FAILED(hr) || !raw_source) return {{}, {}, contention(hr) ? "clipboard-busy" : "source-unavailable", hr};
    std::unique_ptr<IDataObject, ComRelease<IDataObject>> source(raw_source);
    return capture_offer(*source, sequence, owner, mode, confirmed_contention);
}
} // namespace

CaptureResult capture_offer(IDataObject& source, DWORD sequence, HWND owner, CaptureMode mode,
                            bool& confirmed_contention) {
    CaptureResult result;
    auto changed = [=] { return !sequence || sequence != GetClipboardSequenceNumber() || GetClipboardOwner() != owner; };
    if (changed()) return {{}, {}, "superseded"};
    OomEvidence evidence;
    if (mode == CaptureMode::local_history) {
        const auto eligibility = local_history_eligibility(&source, evidence);
        confirmed_contention = eligibility == Eligibility::busy;
        if (eligibility == Eligibility::unknown) {
            result.outcome = "privacy-unconfirmed";
            evidence.guard = NativeGuard::privacy_uncertain;
            result.error = absent_clipboard_format(evidence.monitor_query) ? evidence.history_query : evidence.monitor_query;
            result.oom = evidence;
            return result;
        }
        if (eligibility != Eligibility::allow)
            return {{}, {}, confirmed_contention ? "clipboard-busy" : "excluded-marker"};
    }
    IEnumFORMATETC* raw_formats{};
    auto hr = source.EnumFormatEtc(DATADIR_GET, &raw_formats);
    if (hr == E_OUTOFMEMORY && mode == CaptureMode::local_history) {
        result = recover_native_unicode(sequence, owner, evidence);
        auto process = GetCurrentProcess();
        evidence.user_objects = GetGuiResources(process, GR_USEROBJECTS);
        evidence.gdi_objects = GetGuiResources(process, GR_GDIOBJECTS);
        GetProcessHandleCount(process, &evidence.handles);
        result.oom = evidence;
        return result;
    }
    if (FAILED(hr) || !raw_formats)
        return {{}, {}, contention(hr) ? "clipboard-busy" : "enumeration-failed", hr};
    std::unique_ptr<IEnumFORMATETC, ComRelease<IEnumFORMATETC>> formats(raw_formats);
    bool partial = false;
    struct Offer { FORMATETC format; bool has_device; };
    std::vector<Offer> offers;
    for (size_t count = 0; !changed(); ++count) {
        if (count == 256) { partial = true; break; }
        FORMATETC format{};
        ULONG fetched{};
        hr = formats->Next(1, &format, &fetched);
        std::unique_ptr<DVTARGETDEVICE, decltype(&CoTaskMemFree)> device(format.ptd, CoTaskMemFree);
        if (hr == S_FALSE) break;
        if (hr != S_OK || fetched != 1) { result.error = hr; partial = true; break; }
        format.ptd = nullptr;
        offers.push_back({format, device != nullptr});
    }
    // Read the actual list before requesting any companion pixels or streams,
    // regardless of the order in which the producer advertised its formats.
    const auto group = std::find_if(offers.begin(), offers.end(), [](const Offer& offer) {
        const auto& f = offer.format;
        return !offer.has_device && f.cfFormat == CF_HDROP && (f.tymed & TYMED_HGLOBAL) &&
               f.dwAspect == DVASPECT_CONTENT && f.lindex == -1;
    });
    std::string group_status;
    bool image_files = false;
    if (mode != CaptureMode::format_probe && group != offers.end() && !changed()) {
        group_status = extract(&source, group->format, result, mode, changed);
        image_files = group_status == "saved" && image_file_group(file_group_paths(result.saved.front()));
    }
    for (auto it = offers.begin(); it != offers.end() && !changed(); ++it) {
        const auto& f = it->format;
        std::string status;
        if (it == group && !group_status.empty()) status = group_status;
        else if (image_files && !is_file_group_format(f.cfFormat)) status = "unsupported-path-companion";
        else if (it->has_device) status = "unsupported-medium";
        else status = extract(&source, f, result, mode, changed);
        result.formats.push_back({f.cfFormat, f.tymed, f.dwAspect, f.lindex, std::move(status)});
    }
    if (mode != CaptureMode::format_probe && !changed()) {
        if (!image_files) {
            save_snip_dib(result, owner, changed);
            capture_virtual_streams(source, result, changed);
        }
        finalize_file_group_capture(result);
        for (const auto& f : result.saved) {
            if (f.format != CF_HDROP) continue;
            const auto paths = file_group_paths(f);
            if (paths.size() == 1) result.path_kind = win::local_path_kind(paths.front());
            break;
        }
        const auto filename = RegisterClipboardFormatW(L"FileNameW");
        const auto offered = std::find_if(offers.begin(), offers.end(), [filename](const auto& item) {
            return item.format.cfFormat == filename && !item.has_device && (item.format.tymed & TYMED_HGLOBAL) &&
                   item.format.dwAspect == DVASPECT_CONTENT && item.format.lindex == -1;
        });
        const auto png = RegisterClipboardFormatW(L"PNG");
        const auto pixels = std::find_if(result.saved.begin(), result.saved.end(), [png](const auto& f) {
            return f.format == png || f.format == CF_DIBV5 || f.format == CF_DIB;
        });
        if (group == offers.end() && offered != offers.end() && pixels != result.saved.end() && !changed()) {
            auto format = offered->format;
            format.tymed = TYMED_HGLOBAL;
            Medium medium;
            if (SUCCEEDED(source.GetData(&format, &medium.value)) && medium.value.tymed == TYMED_HGLOBAL) {
                const auto bytes = GlobalSize(medium.value.hGlobal);
                if (bytes >= 2 && bytes <= 65536 && bytes % 2 == 0) {
                    GlobalView view(medium.value.hGlobal);
                    if (view.data) {
                        const auto start = reinterpret_cast<const wchar_t*>(view.data);
                        const auto end = std::find(start, start + bytes / 2, L'\0');
                        std::wstring path(start, end);
                        if (end != start + bytes / 2 && disk_path(path) && image_file_group({path}))
                            result.screenshot_path = std::move(path);
                    }
                }
            }
        }
    }
    if (changed()) {
        result.saved.clear();
        for (auto& observation : result.formats)
            if (observation.outcome == "saved") observation.outcome = "discarded-superseded";
        result.outcome = "superseded";
    } else {
        partial |= std::any_of(result.formats.begin(), result.formats.end(), [](const auto& row) {
            return row.outcome != "saved" && row.outcome != "duplicate" && row.outcome != "unsupported-format" &&
                   row.outcome != "unsupported-orphan-effect" && row.outcome != "unsupported-path-companion";
        });
        result.outcome = result.saved.empty() ? "no-usable-format" : (partial ? "partial" : "captured");
        // Cross-check only an unexpectedly empty inventory. A real unsupported
        // format or a rejected representation is a terminal adapter result.
        if (result.formats.empty() && (contention(result.error) || GetOpenClipboardWindow() ||
            (SUCCEEDED(result.error) && (IsClipboardFormatAvailable(CF_UNICODETEXT) ||
             IsClipboardFormatAvailable(RegisterClipboardFormatW(L"HTML Format"))))))
            result.outcome = "clipboard-busy";
    }
    return result;
}
CaptureResult capture_clipboard(DWORD sequence, CaptureMode mode, ULONGLONG deadline_tick, const AcquireClipboard& acquire) {
    const HWND owner = GetClipboardOwner();
    constexpr unsigned retries = 8;
    unsigned reacquisitions{};
    bool recovering_oom{};
    OomEvidence last_failure;
    for (unsigned i = 0; ; ++i) {
        if (deadline_tick && GetTickCount64() >= deadline_tick) return {{}, {}, "expired", S_OK, i, last_failure};
        bool confirmed_contention{};
        bool acquired{};
        // capture_attempt releases its IDataObject and enumerator before an
        // eligible repeated call obtains a fresh proxy from OleGetClipboard.
        auto result = capture_attempt(sequence, owner, mode, confirmed_contention, acquire, acquired);
        if (recovering_oom && acquired) ++reacquisitions;
        result.retries = i;
        if (result.oom.guard != NativeGuard::none) last_failure = result.oom;
        last_failure.reacquisitions = reacquisitions;
        result.oom = last_failure;
        if (last_failure.guard != NativeGuard::none &&
            (sequence != GetClipboardSequenceNumber() || owner != GetClipboardOwner()))
            return {{}, {}, "superseded", S_OK, i, last_failure};
        if (deadline_tick && GetTickCount64() >= deadline_tick)
            return {{}, {}, "expired", S_OK, i, last_failure};
        if (result.outcome == "enumeration-failed" && result.error == E_OUTOFMEMORY && result.saved.empty() &&
            result.formats.empty() && mode == CaptureMode::local_history && deadline_tick && owner &&
            reacquisitions < 2 && GetTickCount64() + 125 < deadline_tick && sequence == GetClipboardSequenceNumber() &&
            owner == GetClipboardOwner()) {
            recovering_oom = true;
            Sleep(25);
            continue;
        }
        const bool busy_format = std::any_of(result.formats.begin(), result.formats.end(),
            [](const auto& observation) { return observation.outcome == "clipboard-busy"; });
        if (recovering_oom && reacquisitions >= 2) return result;
        if ((!busy_format && result.outcome != "clipboard-busy") || !owner || result.outcome == "superseded")
            return result;
        // Older diagnostic probes and ambiguous empty inventories retain their
        // short retry window. A confirmed lock may use the remaining live ticket.
        if (i >= retries && (!deadline_tick || !(confirmed_contention || busy_format || contention(result.error))))
            return result;
        const DWORD pause = i < retries ? 12 : 32;
        // Leave time to serialize, notify the host and apply the admission checks.
        if (deadline_tick && GetTickCount64() + pause + 100 >= deadline_tick) return result;
        Sleep(pause);
        // Each attempt reacquires one object and checks history privacy markers
        // again. Discard all bytes from the interrupted attempt.
    }
}

struct CaptureSta::Worker {
    std::mutex mutex;
    std::deque<std::packaged_task<CaptureResult()>> queue;
    std::unique_ptr<void, decltype(&CloseHandle)> wake{CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle};
    bool stopping{};
    std::thread thread;

    Worker() {
        if (!wake) throw std::runtime_error("Capture STA event creation failed");
        std::promise<HRESULT> ready;
        auto status = ready.get_future();
        thread = std::thread([this, startup = std::move(ready)]() mutable {
            const auto hr = OleInitialize(nullptr);
            if (FAILED(hr)) { startup.set_value(hr); return; }
            startup.set_value(hr);
            HANDLE event = wake.get();
            for (;;) {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                std::packaged_task<CaptureResult()> task;
                {
                    std::lock_guard lock(mutex);
                    if (!queue.empty()) {
                        task = std::move(queue.front());
                        queue.pop_front();
                    } else if (stopping) break;
                }
                if (task.valid()) task();
                else MsgWaitForMultipleObjectsEx(1, &event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            OleUninitialize();
        });
        if (FAILED(status.get())) { thread.join(); throw std::runtime_error("Capture STA initialization failed"); }
    }
    ~Worker() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        SetEvent(wake.get());
        thread.join();
    }
};

CaptureSta::CaptureSta() : worker_(std::make_unique<Worker>()) {}
CaptureSta::~CaptureSta() = default;

std::future<CaptureResult> CaptureSta::submit(DWORD sequence, CaptureMode mode) {
    std::lock_guard lock(worker_->mutex);
    if (worker_->queue.size() >= 4) {
        std::promise<CaptureResult> full;
        full.set_value({{}, {}, "queue-full"});
        return full.get_future();
    }
    std::packaged_task<CaptureResult()> task([sequence, mode] { return capture_clipboard(sequence, mode); });
    auto future = task.get_future();
    worker_->queue.push_back(std::move(task));
    SetEvent(worker_->wake.get());
    return future;
}
} // namespace cliprope
