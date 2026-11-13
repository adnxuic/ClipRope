#include "../clipboard_sources/external_copy.hpp"
#include "../clipboard_sources/sample.hpp"
#include "../clipboard_sources/virtual_sample.hpp"
#include "history_fixture.hpp"
#include "resident.hpp"
#include "platform_win/workspace_commands.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/clipboard_object.hpp"
#include "platform_win/file_group.hpp"
#include "capture_worker/virtual_streams.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <exception>
#include <set>
#include <fstream>
#include <thread>

namespace {
using namespace cliprope;
using probe::require;
using Microsoft::WRL::ComPtr;
using persistence::HistoryStore;

void locked_flush(const wchar_t* producer) {
    ClipboardPublisher publisher;
    const auto data = sample::formats(false);
    require(SUCCEEDED(publisher.publish(data)), "Could not publish flush fixture");
    using Event = std::unique_ptr<void, decltype(&CloseHandle)>;
    Event locked(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    Event proceed(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    Event finished(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    require(locked && proceed && finished, "Could not create test rendezvous");
    bool read{};
    std::jthread native([&] {
        HWND window = CreateWindowExW(0, L"STATIC", L"Native reader", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        bool opened{};
        for (int i = 0; window && !opened && i < 50; ++i) {
            opened = OpenClipboard(window) != FALSE;
            if (!opened) Sleep(5);
        }
        if (opened) {
            SetEvent(locked.get());
            if (WaitForSingleObject(proceed.get(), 2000) == WAIT_OBJECT_0) {
                const auto h = GetClipboardData(CF_UNICODETEXT);
                auto p = h ? static_cast<const unsigned char*>(GlobalLock(h)) : nullptr;
                read = p && GlobalSize(h) >= data[0].bytes.size() && std::equal(data[0].bytes.begin(), data[0].bytes.end(), p);
                if (p) GlobalUnlock(h);
            }
            CloseClipboard();
        }
        if (window) DestroyWindow(window);
        SetEvent(finished.get());
    });
    probe::pump_until([&] { return WaitForSingleObject(locked.get(), 0) == WAIT_OBJECT_0 ||
                                    WaitForSingleObject(finished.get(), 0) == WAIT_OBJECT_0; }, "Reader did not start");
    SetEvent(proceed.get());
    const auto hr = publisher.flush_if_current();
    probe::pump_until([&] { return WaitForSingleObject(finished.get(), 0) == WAIT_OBJECT_0; }, "Reader could not render");
    native.join();
    probe::ExternalCopy cleanup(producer, L"normal");
    require(read && hr == S_OK, "Flush did not service native reader before shutdown");
    std::cout << "flush=success pending-native-read=correct\n";
}

void bounds() {
    auto files = sample::virtual_group();
    require(SUCCEEDED(prepare_publication(files)), "Valid virtual group rejected");
    ComPtr<IDataObject> source;
    source.Attach(new ClipboardObject(files));
    ComPtr<IEnumFORMATETC> enumeration;
    require(SUCCEEDED(source->EnumFormatEtc(DATADIR_GET, &enumeration)), "Provider enumeration failed");
    std::set<LONG> indices;
    FORMATETC descriptor{};
    ULONG fetched{};
    while (enumeration->Next(1, &descriptor, &fetched) == S_OK) {
        if (descriptor.cfFormat == virtual_files::contents_format()) {
            require(descriptor.tymed == TYMED_ISTREAM, "Provider advertised the wrong medium");
            indices.insert(descriptor.lindex);
        }
        CoTaskMemFree(descriptor.ptd);
    }
    require(indices == std::set<LONG>{0, 1}, "Provider did not enumerate the two actual indices");
    auto malformed = files;
    malformed.pop_back();
    require(FAILED(prepare_publication(malformed)), "Incomplete stream set accepted");
    malformed = files;
    malformed.back().index = 0;
    require(FAILED(prepare_publication(malformed)), "Duplicate index accepted");
    malformed = files;
    malformed.back().bytes.push_back(0);
    require(FAILED(prepare_publication(malformed)), "Mismatched descriptor length accepted");
    malformed = files;
    constexpr size_t name = offsetof(FILEGROUPDESCRIPTORW, fgd) + offsetof(FILEDESCRIPTORW, cFileName);
    malformed.front().bytes[name] = '/';
    require(FAILED(prepare_publication(malformed)), "Virtual path accepted as a file name");
    malformed = files;
    malformed.front().bytes.pop_back();
    require(FAILED(prepare_publication(malformed)), "Truncated descriptor array accepted");
    malformed = files;
    malformed.back().codec_version = 99;
    require(FAILED(prepare_publication(malformed)), "Unknown codec version accepted");
    const wchar_t names[] = L"C:\\ClipRopeFixture\\photo.png\0";
    DROPFILES drop{}; drop.pFiles = sizeof(drop); drop.fWide = TRUE;
    Representation reference{CF_HDROP, std::vector<unsigned char>(sizeof(drop) + sizeof(names))};
    std::memcpy(reference.bytes.data(), &drop, sizeof(drop));
    std::memcpy(reference.bytes.data() + sizeof(drop), names, sizeof(names));
    require(validate_file_group(reference), "Invalid file reference fixture");
    CaptureResult references;
    references.saved = {reference, files.front()};
    references.formats = {{CF_HDROP, TYMED_HGLOBAL, 1, -1, "saved"},
        {files.front().format, TYMED_HGLOBAL, 1, -1, "saved"}, {files[1].format, TYMED_ISTREAM, 1, -1, "indexed-offer"}};
    class NoRead final : public ClipboardObject {
    public:
        NoRead() : ClipboardObject({}) {}
        HRESULT STDMETHODCALLTYPE GetData(FORMATETC*, STGMEDIUM*) override { ++calls; return E_FAIL; }
        unsigned calls{};
    } guard;
    capture_virtual_streams(guard, references, [] { return false; });
    require(!guard.calls && references.saved == std::vector<Representation>{reference} &&
            references.formats[1].outcome == "unsupported-path-companion" &&
            references.formats[2].outcome == "unsupported-path-companion", "File copy materialized companion file contents");
    std::cout << "provider-indices=0,1; malformed-array/path/length/index/codec rejected; physical-file-content-reads=0\n";
}

DWORD child(std::vector<std::wstring> args, const std::function<void()>& started = {}) {
    HANDLE process = probe::launch(args, probe::desktop_path());
    require(process, "Could not launch fixture process");
    if (started) started();
    probe::pump_until([&] { return WaitForSingleObject(process, 0) == WAIT_OBJECT_0; }, "Child command timed out");
    DWORD code{};
    require(GetExitCodeProcess(process, &code) != FALSE, "Child exit code unavailable");
    CloseHandle(process);
    return code;
}

void inspect(std::wstring_view mode) {
    if (mode == L"inspect-empty") {
        require(!IsClipboardFormatAvailable(CF_UNICODETEXT) && !IsClipboardFormatAvailable(CF_HDROP) &&
                !IsClipboardFormatAvailable(virtual_files::descriptor_format()) && !IsClipboardFormatAvailable(virtual_files::contents_format()),
                "Virtual-only exit left an unreadable format");
        return;
    }
    ComPtr<IDataObject> object;
    require(SUCCEEDED(OleGetClipboard(&object)), "Cannot open republished object");
    ComPtr<IEnumFORMATETC> formats;
    require(SUCCEEDED(object->EnumFormatEtc(DATADIR_GET, &formats)), "Cannot enumerate republished formats");
    std::vector<Representation> expected{sample::formats(false).front()};
    if (mode == L"inspect-only") expected.clear();
    if (mode == L"inspect-scalar") expected = sample::formats(false);
    const bool virtual_group = mode == L"inspect-full" || mode == L"inspect-only" || mode == L"inspect-rich-group";
    if (mode == L"inspect-rich-group") expected = sample::formats(false);
    if (virtual_group) {
        auto files = sample::virtual_group();
        expected.insert(expected.end(), files.begin(), files.end());
    }
    std::set<std::pair<UINT, LONG>> offered_keys;
    bool indexed_family{};
    FORMATETC offered{};
    ULONG fetched{};
    while (formats->Next(1, &offered, &fetched) == S_OK) {
        std::unique_ptr<DVTARGETDEVICE, decltype(&CoTaskMemFree)> device(offered.ptd, CoTaskMemFree);
        require(offered.cfFormat != CF_HDROP, "Virtual file names were exposed as paths");
        require(offered.cfFormat != RegisterClipboardFormatW(L"ClipRope Synthetic Private"), "Unsupported format was republished");
        if (offered.cfFormat != CF_UNICODETEXT && offered.cfFormat != RegisterClipboardFormatW(L"HTML Format") &&
            offered.cfFormat != virtual_files::descriptor_format() && offered.cfFormat != virtual_files::contents_format()) continue;
        std::cout << "external cf=" << offered.cfFormat << " index=" << offered.lindex << " medium=" << offered.tymed
                  << " aspect=" << offered.dwAspect << '\n';
        require(offered.dwAspect == DVASPECT_CONTENT && !offered.ptd, "Wrong published aspect or target");
        if (offered.cfFormat == virtual_files::contents_format() && offered.lindex == -1) {
            require(virtual_group, "Unexpected virtual stream family");
            indexed_family = true;
            continue;
        }
        require(std::any_of(expected.begin(), expected.end(), [&](const auto& f) {
            return f.format == offered.cfFormat && f.index == offered.lindex && (offered.tymed & f.tymed);
        }), "Unexpected published representation");
        offered_keys.emplace(offered.cfFormat, offered.lindex);
    }
    for (const auto& item : expected) {
        require(offered_keys.contains({item.format, item.index}) || (indexed_family && item.format == virtual_files::contents_format()),
                "Expected format or indexed family not enumerated");
        FORMATETC exact{static_cast<CLIPFORMAT>(item.format), nullptr, item.aspect, item.index, item.tymed};
        STGMEDIUM medium{};
        require(SUCCEEDED(object->GetData(&exact, &medium)), "Advertised representation unreadable");
        struct Release { STGMEDIUM& value; ~Release() { ReleaseStgMedium(&value); } } release{medium};
        require(medium.tymed == item.tymed, "Format lost its original medium");
        if (medium.tymed == TYMED_ISTREAM) {
            std::vector<unsigned char> bytes(item.bytes.size() + 1);
            ULONG size{};
            require(SUCCEEDED(medium.pstm->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &size)), "Restored stream read failed");
            bytes.resize(size);
            require(bytes == item.bytes, "Restored indexed bytes differ");
        } else {
            require(GlobalSize(medium.hGlobal) >= item.bytes.size(), "Restored global too small");
            auto* bytes = static_cast<const unsigned char*>(GlobalLock(medium.hGlobal));
            const bool equal = bytes && std::equal(item.bytes.begin(), item.bytes.end(), bytes);
            if (bytes) GlobalUnlock(medium.hGlobal);
            require(equal, "Restored descriptor names or Unicode bytes differ");
        }
    }
    FORMATETC bad{static_cast<CLIPFORMAT>(virtual_files::contents_format()), nullptr, DVASPECT_CONTENT, 2, TYMED_ISTREAM};
    STGMEDIUM invalid{};
    require(FAILED(object->GetData(&bad, &invalid)), "Out-of-range virtual index readable");
}

struct ReplaceDuringFlush {
    win::HistoryClipboard& history;
    const wchar_t* producer;
    HANDLE unlock{}, released{};
    std::unique_ptr<probe::ExternalCopy> newer;
    std::exception_ptr failure;
    HWND owner{};
    DWORD sequence{};
    static LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        if (msg != WM_RENDERALLFORMATS) return DefWindowProcW(window, msg, w, l);
        auto& state = *reinterpret_cast<ReplaceDuringFlush*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        try {
            require(state.history.flush_current().result == RPC_E_CALL_REJECTED, "Nested flush did not see the active STA flush");
            SetEvent(state.unlock);
            require(WaitForSingleObject(state.released, 1500) == WAIT_OBJECT_0, "Contention holder did not release clipboard");
            state.newer = std::make_unique<probe::ExternalCopy>(state.producer, L"second");
            state.owner = GetClipboardOwner();
            state.sequence = GetClipboardSequenceNumber();
        } catch (...) { state.failure = std::current_exception(); }
        return 0;
    }
};

void contend_virtual_flush(win::HistoryClipboard& history, application::PublicationId generation,
                           const wchar_t* producer, bool replace) {
    using Event = std::unique_ptr<void, decltype(&CloseHandle)>;
    Event ready(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    Event start(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    Event unlock(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    Event released(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    require(ready && start && unlock && released, "Cannot allocate native contention barriers");
    ReplaceDuringFlush state{history, producer, unlock.get(), released.get()};
    WNDCLASSW wc{};
    wc.lpfnWndProc = ReplaceDuringFlush::proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClipRope.FlushRace.Test";
    require(RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Cannot register flush STA callback");
    const HWND window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, &state);
    require(window, "Cannot create private flush STA window");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } receiver{window};
    const auto before = GetClipboardSequenceNumber();
    bool locked{};
    std::jthread holder([&] {
        locked = OpenClipboard(nullptr) != FALSE;
        SetEvent(ready.get());
        if (!locked) return;
        if (WaitForSingleObject(start.get(), 1500) == WAIT_OBJECT_0 && replace) {
            // A posted render message is processed by the waiting publisher's STA pump.
            PostMessageW(window, WM_RENDERALLFORMATS, 0, 0);
        }
        WaitForSingleObject(unlock.get(), 3000);
        CloseClipboard();
        SetEvent(released.get());
    });
    require(WaitForSingleObject(ready.get(), 1500) == WAIT_OBJECT_0 && locked, "Cannot establish native clipboard contention");
    SetEvent(start.get());
    const auto result = history.flush_current();
    SetEvent(unlock.get());
    holder.join();
    if (state.failure) std::rethrow_exception(state.failure);
    require(result.effect == FlushEffect::untouched && !result.installed && result.companions == 2,
            "Contended/stale flush changed native companions");
    if (replace) {
        require(state.newer && state.owner && state.sequence != before && result.result == S_FALSE &&
                    state.owner == GetClipboardOwner() && state.sequence == GetClipboardSequenceNumber() &&
                    !history.owns_publication(generation) && capture_clipboard(state.sequence).saved == sample::formats(true),
                "A contended outer flush replaced the newer publisher");
        std::cout << "contended replacement kept owner, sequence and native payload; nested flush rejected\n";
    } else {
        require(result.result == CLIPBRD_E_CANT_OPEN && result.summary().find("win32=0") == std::string::npos &&
                    before == GetClipboardSequenceNumber() && history.owns_publication(generation),
                "Blocked virtual flush lost its ownership or diagnostic");
        std::cout << "held reader " << result.summary() << '\n';
    }
}

void partial_install(const wchar_t* self, const wchar_t* producer, const wchar_t* cli, const wchar_t* desktop) {
    history_fixture::Directory root;
    const auto profile = root.path / "profile", log = root.path / "flush.log";
    auto offer = sample::formats(false);
    const auto group = sample::virtual_group();
    offer.insert(offer.end(), group.begin(), group.end());
    domain::Id entry, snapshot;
    {
        HistoryStore seed(profile);
        CaptureResult result{offer, {}, "captured"};
        for (const auto& f : offer) result.formats.push_back({f.format, f.tymed, f.aspect, f.index, "saved"});
        entry = seed.admit("main", win::capture_snapshot(std::move(result)));
        snapshot = seed.entry(entry).snapshot_id;
    }
    for (unsigned position : {1u, 2u}) {
        require(SetEnvironmentVariableW(L"CLIPROPE_PROBE_FLUSH_SET", std::to_wstring(position).c_str()),
                "Cannot configure private flush failure");
        if (position == 1) {
            // Capture the actual headless command's content-free shutdown report.
            const auto file = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(file != INVALID_HANDLE_VALUE, "Could not capture headless diagnostics");
            const auto previous = GetStdHandle(STD_ERROR_HANDLE);
            struct Restore { HANDLE previous, file; ~Restore() { SetStdHandle(STD_ERROR_HANDLE, previous); CloseHandle(file); } } restore{previous, file};
            require(SetStdHandle(STD_ERROR_HANDLE, file), "Could not redirect fixture diagnostics");
            require(child({cli, profile.wstring(), L"head"}) == 1, "Failed headless flush returned success");
        } else {
            probe::Resident host(desktop, profile, true, log);
            SendMessageW(host.workspace, WM_COMMAND, WPARAM(win::WorkspaceCommand::head), 0);
            require(IsClipboardFormatAvailable(virtual_files::descriptor_format()), "Desktop head did not publish virtual files");
            require(PostMessageW(host.listener(), WM_CLOSE, 0, 0), "Could not request desktop exit");
            probe::pump_until([&] { return WaitForSingleObject(host.handle(), 0) == WAIT_OBJECT_0; }, "Desktop did not exit");
            DWORD code{};
            require(GetExitCodeProcess(host.handle(), &code) && code == 1, "Failed desktop flush returned success");
        }
        SetEnvironmentVariableW(L"CLIPROPE_PROBE_FLUSH_SET", nullptr);
        std::ifstream report(log);
        const std::string lines{std::istreambuf_iterator<char>(report), {}};
        FlushReceipt expected{CLIPBRD_E_CANT_SET, position == 1 ? FlushEffect::cleared : FlushEffect::partial,
                              position - 1, 2, ERROR_ACCESS_DENIED};
        require(lines.find(expected.summary()) != lines.npos, "Missing exact partial exit report");
        require(!IsClipboardFormatAvailable(RegisterClipboardFormatW(L"HTML Format")) &&
                child({self, position == 1 ? L"inspect-empty" : L"inspect-partial"}) == 0,
                "Actual system clipboard disagrees with failed installation count");
        HistoryStore reopened(profile);
        require(reopened.entry(entry).snapshot_id == snapshot &&
                win::restorable_formats(reopened.load(entry).snapshot) == offer,
                "Failed native flush changed saved streams, metadata or companions");
        win::HistoryClipboard history;
        const auto generation = history.publish(reopened.load(entry));
        require(generation && history.owns_publication(generation) && child({self, L"inspect-rich-group"}) == 0,
                "Saved group could not be recalled after exit");
        probe::ExternalCopy replacement(producer, L"second");
        const auto seq = GetClipboardSequenceNumber();
        auto skipped = history.flush_current();
        require(skipped.result == S_FALSE && skipped.effect == FlushEffect::untouched && !skipped.installed &&
                !history.owns_publication(generation) && GetClipboardSequenceNumber() == seq &&
                capture_clipboard(seq).saved == sample::formats(true), "Stale generation replaced the newer clipboard");
        std::cout << expected.summary() << " history-streams=2 recalled=complete\n";
    }
    {
        HistoryStore reopened(profile);
        win::HistoryClipboard history;
        const auto generation = history.publish(reopened.load(entry));
        require(generation && history.owns_publication(generation), "Could not restore the virtual offer for contention");
        contend_virtual_flush(history, generation, producer, false);
        contend_virtual_flush(history, generation, producer, true);
    }
    ClipboardPublisher virtual_only;
    require(SUCCEEDED(virtual_only.publish(group)), "Could not publish virtual-only group");
    const auto empty = virtual_only.flush_current();
    require(empty.result == clipboard_partial_flush && empty.effect == FlushEffect::flattened &&
            empty.installed == 0 && empty.companions == 0 && child({self, L"inspect-empty"}) == 0,
            "Intentional empty flatten was reported as failed");
    probe::ExternalCopy cleanup(producer, L"normal");
}

void contract(const wchar_t* self, const wchar_t* producer, const wchar_t* cli) {
    for (const wchar_t* mode : {L"delayed", L"partial", L"failed"}) {
        const bool delayed = std::wstring_view(mode) == L"delayed", failed = std::wstring_view(mode) == L"failed";
        history_fixture::Directory root;
        {
            probe::ExternalCopy original(producer, mode);
            const auto code = child({cli, root.path.wstring(), L"capture"}, [&] {
                if (!delayed) return;
                probe::pump_until([&] { return WaitForSingleObject(original.reading, 0) == WAIT_OBJECT_0; }, "No deferred HTML read");
                SetEvent(original.proceed);
            });
            require((code != 0) == failed, "Incorrect adapter contract admission");
            probe::ExternalCopy overwrite(producer, L"second");
            require(capture_clipboard(GetClipboardSequenceNumber()).saved == sample::formats(true), "Intervening overwrite missing");
        }
        {
            HistoryStore checked(root.path);
            const auto entries = checked.hanging(checked.active_rope());
            require(entries.size() == (failed ? 0 : 1), "Unexpected committed entry count");
            if (failed) continue;
            const auto content = checked.load(entries.front().id);
            unsigned retained{}, failures{}, skipped{};
            for (const auto& f : content.snapshot.formats) {
                if (f.outcome == "saved") {
                    ++retained;
                    require(f.tymed == TYMED_HGLOBAL && f.index == -1 && f.aspect == DVASPECT_CONTENT && f.codec_version == 1,
                            "Wrong durable scalar medium");
                } else if (f.outcome == "failed") ++failures;
                else if (f.outcome == "skipped") ++skipped;
            }
            require(retained == (delayed ? 2u : 1u) && failures == (delayed ? 0u : 1u) && skipped == 1,
                    "Skipped and failed formats not distinguished");
        }
        const auto publish_status = child({cli, root.path.wstring(), L"head"});
        const auto inspect_result = publish_status ? 1u : child({self, delayed ? L"inspect-scalar" : L"inspect-partial"});
        probe::ExternalCopy cleanup(producer, L"normal");
        require(publish_status == 0, "History restart failed to publish");
        require(inspect_result == 0, "Flushed scalar publication failed verification");
    }
    std::cout << "delayed=2 retained; partial=1 retained,1 failed,1 skipped; total failure=0 entries\n";
}

void run(const wchar_t* self, const wchar_t* producer, const wchar_t* cli, const wchar_t* mode) {
    const bool only = std::wstring_view(mode) == L"virtual-only";
    const bool full = only || std::wstring_view(mode) == L"virtual";
    const bool empty = std::wstring_view(mode) == L"virtual-only-broken";
    history_fixture::Directory root;
    {
        probe::ExternalCopy original(producer, mode);
        const auto status = child({cli, root.path.wstring(), L"capture"});
        require((status != 0) == empty, "Capture admission status differs from recoverability");
        probe::ExternalCopy overwrite(producer, L"second");
        require(capture_clipboard(GetClipboardSequenceNumber()).saved == sample::formats(true), "Overwrite was not applied");
    }
    // Original producer and capture host have exited. Only the reopened store
    // supplies the publication seen by a third process.
    {
        HistoryStore store(root.path);
        const auto entries = store.hanging(store.active_rope());
        require(entries.size() == (empty ? 0 : 1), "Unexpected durable entry count");
        if (empty) return;
        auto loaded = store.load(entries.front().id);
        const auto& snapshot = loaded.snapshot;
        require(snapshot.capture_status == (full ? "captured" : "partial"), "Incorrect durable capture status");
        size_t saved{}, failed{};
        bool failed_stream{}, withdrawn_group{};
        for (const auto& item : snapshot.formats) {
            if (item.outcome != "saved") {
                failed += item.outcome == "failed";
                failed_stream |= item.outcome == "failed" && item.name == "FileContents" && item.index == 1;
                withdrawn_group |= item.outcome == "failed" && item.name == "FileGroupDescriptorW";
                continue;
            }
            ++saved;
            require(item.codec_version == 1, "Lost codec version");
            if (item.tymed == TYMED_ISTREAM) require(item.name == "FileContents" && item.standard_id == 0 &&
                item.aspect == DVASPECT_CONTENT && (item.index == 0 || item.index == 1), "Durable stream identity is invalid");
        }
        require(saved == (only ? 3 : full ? 4 : 1) && (full ? failed == 0 : failed >= 1), "Durable outcomes incorrect");
        require(full || (failed_stream && withdrawn_group), "Incomplete descriptor or failed indexed stream absent from inventory");
        win::HistoryClipboard owner;
        auto invoke = application::bind_invocation(store, application::Route::rope_head, entries.front().id);
        const auto result = invoke.run(store, [&](const auto& content) { return owner.publish(content); });
        const auto status = result.publication == application::Publication::published ?
            child({self, only ? L"inspect-only" : full ? L"inspect-full" : L"inspect-partial"}) : 1u;
        probe::ExternalCopy cleanup(producer, L"second");
        require(owner.flush_if_current() == S_FALSE && capture_clipboard(GetClipboardSequenceNumber()).saved == sample::formats(true),
                "Shutdown replaced a newer source");
        require(result.publication == application::Publication::published, "History publication failed");
        require(status == 0, "Cross-process verification failed");
        std::cout << "saved=" << saved << " failed=" << failed << " cross-process bytes, names, indices and media verified\n";
    }
    if (full) {
        // The native clipboard cannot hold a distinct handle for each index.
        // Test the actual process exit, including destruction of the OLE window.
        const auto publish_status = child({cli, root.path.wstring(), L"head"});
        const auto after = publish_status ? 1u : child({self, only ? L"inspect-empty" : L"inspect-partial"});
        probe::ExternalCopy cleanup(producer, L"normal");
        require(!publish_status && !after, "Explicit exit left a broken virtual group or lost its scalar fallback");
        std::cout << "owner-exited; no stale virtual descriptors; portable-formats=" << (only ? 0 : 1) << '\n';
    }
}
}

int wmain(int argc, wchar_t** argv) {
    const bool in_memory = argc == 2 && std::wstring_view(argv[1]) == L"--bounds";
    if ((!in_memory && !probe::private_station()) || FAILED(OleInitialize(nullptr))) return 2;
    int result{};
    try {
        if (in_memory) bounds();
        else if (argc == 2) inspect(argv[1]);
        else if (argc == 3 && std::wstring_view(argv[1]) == L"--locked-flush") locked_flush(argv[2]);
        else if (argc == 4 && std::wstring_view(argv[1]) == L"--contract") contract(argv[0], argv[2], argv[3]);
        else if (argc == 5 && std::wstring_view(argv[1]) == L"--partial-install")
            partial_install(argv[0], argv[2], argv[3], argv[4]);
        else if (argc == 3) {
            for (const auto mode : {L"virtual", L"virtual-only", L"virtual-broken", L"virtual-large", L"virtual-only-broken"})
                run(argv[0], argv[1], argv[2], mode);
        } else result = 2;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    OleUninitialize();
    return result;
}
