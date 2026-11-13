#include "capture_worker/capture.hpp"
#include "platform_win/clipboard_object.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "../clipboard_sources/sample.hpp"
#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/history_clipboard.hpp"
#include <algorithm>
#include <cstring>

namespace {
using namespace cliprope;
using probe::require;
class NativeSource {
public:
    HWND owner = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    ~NativeSource() {
        if (GetClipboardOwner() == owner && OpenClipboard(owner)) {
            if (GetClipboardOwner() == owner) EmptyClipboard();
            CloseClipboard();
        }
        DestroyWindow(owner);
    }
    DWORD put(const std::vector<Representation>& values) const {
        require(owner && OpenClipboard(owner), "Native source could not open clipboard");
        struct Unlock { ~Unlock() { CloseClipboard(); } } unlock;
        require(EmptyClipboard() != FALSE, "Native source could not replace its sample");
        for (const auto& item : values) {
            auto memory = GlobalAlloc(GMEM_MOVEABLE, item.bytes.size());
            auto bytes = GlobalLock(memory);
            require(bytes, "Native source allocation failed");
            std::memcpy(bytes, item.bytes.data(), item.bytes.size());
            GlobalUnlock(memory);
            require(SetClipboardData(item.format, memory) != nullptr, "Native fixture format unavailable");
        }
        return GetClipboardSequenceNumber();
    }
};
class FailedEnumerator final : public ClipboardObject {
public:
    explicit FailedEnumerator(std::vector<Representation> markers = {}) : ClipboardObject(std::move(markers)) {}
    HRESULT reason = E_OUTOFMEMORY;
    UINT ambiguous_marker{};
    HRESULT marker_error{};
    unsigned calls{};
    unsigned* released{};
    unsigned* body_reads{};
    unsigned* ambiguous_queries{};
    unsigned* enumerations{};
    IDataObject* privacy_source{};
    std::function<void()> before_failure;
    std::function<void()> on_release;
    ~FailedEnumerator() override { if (released) ++*released; if (on_release) on_release(); }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (body_reads && format && format->cfFormat == CF_UNICODETEXT) ++*body_reads;
        return ClipboardObject::GetData(format, medium);
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (format && ambiguous_marker && format->cfFormat == ambiguous_marker) {
            if (ambiguous_queries) ++*ambiguous_queries;
            return marker_error;
        }
        if (privacy_source) {
            const auto hr = privacy_source->QueryGetData(format);
            std::cout << "raw-proxy marker-query hr=0x" << std::hex << static_cast<DWORD>(hr) << std::dec << '\n';
            return hr;
        }
        return ClipboardObject::QueryGetData(format);
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** value) override {
        ++calls;
        if (enumerations) ++*enumerations;
        *value = nullptr;
        if (before_failure) before_failure();
        if (reason == S_OK) return ClipboardObject::EnumFormatEtc(direction, value);
        return reason;
    }
};

void reacquisition(NativeSource& source, const wchar_t* reader_exe) {
    const auto formats = sample::formats(false);
    auto offered_payload = formats;
    const auto monitor = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    source.put(formats); // A rich native inventory cannot use Unicode-only recovery.
    unsigned acquired{}, released{}, reads{};
    using Configure = std::function<void(FailedEnumerator&, unsigned)>;
    auto run = [&](const Configure& configure, ULONGLONG budget = 2800) {
        acquired = released = reads = 0;
        auto result = capture_clipboard(GetClipboardSequenceNumber(), CaptureMode::local_history,
            GetTickCount64() + budget, [&](IDataObject** output) -> HRESULT {
                require(acquired == released, "Previous OLE proxy still alive at reacquisition");
                ++acquired;
                auto offer = new FailedEnumerator(offered_payload);
                offer->released = &released;
                offer->body_reads = &reads;
                configure(*offer, acquired);
                *output = offer;
                return S_OK;
            });
        require(acquired == released, "Capture retained an OLE proxy");
        return result;
    };
    history_fixture::Directory directory;
    persistence::HistoryStore history(directory.path);
    application::CaptureCoordinator coordinator(history);
    application::CaptureOutcome pending;
    auto ticket = coordinator.begin(GetClipboardSequenceNumber(), "source.exe", false, {}, pending);
    require(ticket.has_value(), "Ticket creation failed");
    const auto recovered = run([](auto& offer, unsigned n) { if (n == 2) offer.reason = S_OK; });
    require(recovered.saved == formats && acquired == 2 && reads == 1 && recovered.oom.reacquisitions == 1 &&
            recovered.oom.guard == NativeGuard::mixed, "Fresh OLE object did not restore the full offer");
    auto outcome = coordinator.finish(*ticket, GetClipboardSequenceNumber(), win::capture_snapshot(recovered));
    require(!outcome.entry.empty() && coordinator.finish(*ticket, GetClipboardSequenceNumber(),
            win::capture_snapshot(recovered)).status == "abandoned" && history.hanging(ticket->rope).size() == 1,
            "Recovered worker output did not admit exactly once");
    const auto include = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    for (const auto marker : {monitor, include}) {
        for (const HRESULT hr : {DV_E_CLIPFORMAT, DV_E_FORMATETC, E_ACCESSDENIED, E_OUTOFMEMORY, E_FAIL, S_FALSE,
                                  CLIPBRD_E_CANT_OPEN}) {
            unsigned queries{}, enumerations{};
            const auto result = run([&](auto& offer, unsigned n) {
                offer.ambiguous_queries = &queries;
                offer.enumerations = &enumerations;
                offer.ambiguous_marker = marker;
                // The contending producer becomes available on its second acquisition.
                offer.marker_error = hr == CLIPBRD_E_CANT_OPEN && n > 1 ? DV_E_FORMATETC : hr;
                offer.reason = S_OK;
            });
            if (hr == DV_E_FORMATETC || hr == DV_E_CLIPFORMAT || hr == CLIPBRD_E_CANT_OPEN) {
                require(result.outcome == "captured" && result.saved == formats && enumerations == 1 && reads == 1 &&
                        queries == (hr == CLIPBRD_E_CANT_OPEN ? 2u : 1u), "Recognized absence or contention lost content");
            } else {
                // Use a fresh sequence for a distinct coordinator ticket.
                const auto sequence = source.put(formats);
                const auto denied = coordinator.begin(sequence, "source.exe", false, {}, pending);
                require(denied && coordinator.finish(*denied, sequence, win::capture_snapshot(result)).status == "privacy-unconfirmed" &&
                        history.hanging(ticket->rope).size() == 1 && result.error == hr && result.saved.empty() &&
                        !enumerations && !reads && acquired == 1 && queries == 1, "Unknown marker enumerated, read or admitted content");
            }
        }
    }
    const auto permanent = run([](auto&, unsigned) {});
    require(permanent.outcome == "enumeration-failed" && permanent.saved.empty() && !reads &&
            acquired == 3 && permanent.oom.reacquisitions == 2, "Permanent enumeration failure exceeded retry limit");
    const auto unknown = run([&](auto& offer, unsigned n) {
        if (n == 2) { offer.ambiguous_marker = monitor; offer.marker_error = E_ACCESSDENIED; offer.reason = S_OK; }
    });
    require(unknown.outcome == "privacy-unconfirmed" && unknown.saved.empty() && !reads && acquired == 2 &&
            unknown.oom.monitor_query == E_ACCESSDENIED, "Uncertain fresh privacy verdict read a payload");
    const auto excluded = run([&](auto& offer, unsigned n) {
        if (n == 2) { offer.ambiguous_marker = monitor; offer.marker_error = S_OK; offer.reason = S_OK; }
    });
    require(excluded.outcome == "excluded-marker" && excluded.saved.empty() && !reads && acquired == 2,
            "Fresh source exclusion entered history");
    const auto expired = run([](auto& offer, unsigned) { offer.before_failure = [] { Sleep(60); }; }, 35);
    require(expired.outcome == "expired" && expired.saved.empty() && acquired == 1 && !reads,
            "Retried an expired OLE enumeration");
    const auto replacement = run([&](auto& offer, unsigned n) {
        if (n == 2) { source.put({sample::formats(true).front()}); offer.reason = S_OK; }
    });
    require(replacement.outcome == "superseded" && replacement.saved.empty() && !reads && acquired == 2,
            "Fresh acquisition read the next owner's copy");
    // Each reader holds its open through old-proxy release. Some Windows
    // configurations allow another OpenClipboard(NULL) while the recorded HWND is NULL.
    source.put({formats.front()});
    offered_payload = {formats.front()};
    for (bool anonymous : {false, true}) {
        auto holder = std::make_unique<probe::ExternalCopy>(reader_exe, anonymous ? L"anonymous-reader" : L"held-reader");
        const auto locked = run([&](auto& offer, unsigned n) {
            if (n == 1) {
                offer.before_failure = [&] {
                    SetEvent(holder->proceed);
                    probe::pump_until([&] { return WaitForSingleObject(holder->reading, 0) == WAIT_OBJECT_0; }, "Test reader failed");
                    require((GetOpenClipboardWindow() == nullptr) == anonymous, "Unexpected reader ownership");
                };
                offer.on_release = [&] { holder.reset(); };
            } else offer.reason = S_OK;
        });
        std::cout << "anonymous=" << anonymous << " OLE objects=" << acquired
                  << " guard=" << win::recovery_guard_name(static_cast<DWORD>(locked.oom.guard))
                  << " native-error=" << locked.oom.native_error << " replay USER/GDI/handles="
                  << locked.oom.user_objects << "/" << locked.oom.gdi_objects << "/" << locked.oom.handles << '\n';
        const bool reopened = acquired == 2 && locked.oom.guard == NativeGuard::open_failed;
        const bool native = anonymous && acquired == 1 && locked.oom.guard == NativeGuard::recovered;
        require(locked.saved == offered_payload && (reopened || native), "External reader prevented stable text recovery");
    }
    std::cout << "fresh-object recovery=once-at-head; permanent=3 acquisitions; unclear/excluded/expired/replaced=0\n";
}
void exercise(const wchar_t* reader_exe) {
    NativeSource source;
    FailedEnumerator failure;
    auto read = [&](FailedEnumerator& object, CaptureMode mode = CaptureMode::local_history) {
        bool confirmed{};
        return capture_offer(object, GetClipboardSequenceNumber(), source.owner, mode, confirmed);
    };
    const auto unicode = sample::formats(false).front();
    source.put({unicode});
    IDataObject* native{};
    require(SUCCEEDED(OleGetClipboard(&native)) && native, "Windows raw clipboard proxy unavailable");
    failure.privacy_source = native;
    auto valid = read(failure);
    failure.privacy_source = nullptr;
    native->Release();
    std::cout << "injected-enum result=" << valid.outcome << " rows=" << valid.formats.size()
              << " saved=" << valid.saved.size() << '\n';
    require(valid.outcome == "partial" && valid.error == E_OUTOFMEMORY && valid.saved == std::vector{unicode} &&
            valid.formats.front().format == CF_UNICODETEXT && valid.formats.front().outcome == "saved",
            "Native recovery did not preserve the exact eligible Unicode");
    require(read(failure, CaptureMode::format_probe).outcome == "enumeration-failed", "Historic probe contract changed");
    failure.reason = E_FAIL;
    require(read(failure).saved.empty(), "Unrelated enumeration error bypassed the adapter");
    failure.reason = E_OUTOFMEMORY;
    source.put(sample::formats(false));
    require(read(failure).outcome == "enumeration-failed", "Mixed rich offer misclassified as native text");
    source.put({{CF_TEXT, {'A', 0}}});
    require(read(failure).outcome == "enumeration-failed", "Synthesized Unicode misclassified as the original format");
    source.put({unicode, {RegisterClipboardFormatW(L"DataObject"), {1}}});
    require(read(failure).saved.empty(), "OLE envelope misclassified as raw text");
    source.put({{CF_UNICODETEXT, {0x00, 0xdc, 0, 0}}});
    require(read(failure).outcome == "no-usable-format", "Malformed UTF-16 passed validation");
    source.put({{CF_UNICODETEXT, std::vector<unsigned char>(representation_byte_limit(CF_UNICODETEXT) + 2)}});
    const auto oversized = read(failure);
    require(oversized.saved.empty() && oversized.formats.front().outcome == "size-rejected", "Oversized native HGLOBAL accepted");
    const Representation exclude{RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), {1}};
    source.put({unicode, exclude});
    auto calls = failure.calls;
    require(read(failure).outcome == "excluded-marker" && failure.calls == calls, "Native exclusion reached the fallback");
    source.put({unicode});
    FailedEnumerator private_offer({exclude});
    require(read(private_offer).outcome == "excluded-marker" && !private_offer.calls,
            "Private OLE exclusion reached enumeration");
    FailedEnumerator no_history({{RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), {0, 0, 0, 0}}});
    require(read(no_history).outcome == "excluded-marker" && !no_history.calls,
            "Explicit zero local-history permission reached enumeration");
    const auto expired = capture_clipboard(GetClipboardSequenceNumber(), CaptureMode::local_history, GetTickCount64() - 1);
    require(expired.outcome == "expired" && expired.saved.empty(), "Expired capture read native content");
    for (const auto marker : {exclude.format, RegisterClipboardFormatW(L"CanIncludeInClipboardHistory")}) {
        for (const auto hr : {E_OUTOFMEMORY, E_ACCESSDENIED}) {
            failure.ambiguous_marker = marker;
            failure.marker_error = hr;
            const auto uncertain = read(failure);
            require(uncertain.outcome == "privacy-unconfirmed" && uncertain.error == hr && uncertain.saved.empty(),
                    "An ambiguous OLE privacy query allowed a native text read");
        }
    }
    failure.ambiguous_marker = 0;
    failure.before_failure = [&] { source.put({sample::formats(true).front()}); };
    auto replaced = read(failure);
    require(replaced.outcome == "superseded" && replaced.saved.empty(), "Replacement entered the old ticket");
    reacquisition(source, reader_exe);
    source.put({unicode});
    auto sequence = GetClipboardSequenceNumber();
    worker::Process diagnostic(1, sequence, GetTickCount64() + 2700);
    probe::pump_until([&] { return diagnostic.ready(); }, "Production diagnostic worker timed out");
    DWORD code{};
    require(diagnostic.take(1, sequence, code).saved == std::vector{unicode} && !code, "Production diagnostic read failed");
    std::cout << "normal native text worker peak private commit KiB=" << diagnostic.peak_commit_kib() << " limit-KiB=262144\n";
    std::cout << "forced-enum-out-of-memory=partial exact-Unicode; mixed/OLE/unrelated-error/malformed/markers/replacement rejected\n";
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int result{};
    try { exercise(argv[1]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    OleUninitialize();
    return result;
}
