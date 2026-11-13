#include "resident.hpp"
#include "diagnostic_report.hpp"
#include "history_observer.hpp"
#include "../clipboard_sources/sample.hpp"
#include "../fault_injection/directory_access.hpp"
#include "persistence/history_store.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
using namespace cliprope;
using probe::require;
using persistence::HistoryStore;
namespace fs = std::filesystem;

LONGLONG tick() {
    LARGE_INTEGER value{};
    require(QueryPerformanceCounter(&value), "Performance counter unavailable");
    return value.QuadPart;
}

double ms(LONGLONG delta) {
    LARGE_INTEGER frequency{};
    require(QueryPerformanceFrequency(&frequency), "Performance counter frequency unavailable");
    return 1000. * delta / frequency.QuadPart;
}

std::vector<LONGLONG> responses(const fs::path& receipt) {
    std::ifstream input(receipt);
    std::vector<LONGLONG> ticks;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("heartbeat ")) continue;
        std::istringstream record(line.substr(10));
        size_t sequence{};
        LONGLONG at{};
        if (!(record >> sequence >> at)) break; // The writer may be flushing this last line.
        require(sequence == ticks.size() + 1, "Heartbeat lost or reordered");
        ticks.push_back(at);
    }
    return ticks;
}

void heartbeat(const wchar_t* exe, const wchar_t* producer, const fs::path& profile) {
    const auto receipt = profile / "trace.txt";
    const auto launch = tick();
    probe::Resident host(exe, profile, false, receipt);
    const auto ready = tick();
    auto observer = history_fixture::observe(profile, SQLITE_OPEN_READWRITE);
    auto count = [&] {
        persistence::Statement entries(observer.get(), "SELECT count(*) FROM entries");
        require(entries.step(), "Cannot count committed entries");
        return entries.integer(0);
    };
    std::vector<LONGLONG> sent;
    auto post = [&] {
        sent.push_back(tick());
        require(PostMessageW(host.listener(), WM_NULL, sent.size(), 0), "Could not post heartbeat");
    };
    post();
    probe::pump_until([&] { return responses(receipt).size() == 1; }, "Idle heartbeat missing");
    probe::ExternalCopy copy(producer, L"delayed");
    probe::pump_until([&] { return WaitForSingleObject(copy.reading, 0) == WAIT_OBJECT_0; }, "Read did not bind");
    const auto seq = GetClipboardSequenceNumber();
    const auto owner = GetClipboardOwner();
    LONGLONG release{}, read_done{};
    {
        persistence::Transaction writer(observer.get());
        read_done = tick();
        SetEvent(copy.proceed);
        // The second connection holds the only writer slot while the worker returns to the desktop.
        for (int n = 0; n < 10; ++n) { post(); Sleep(40); }
        require(count() == 0, "Pending capture became visible before writer release");
        release = tick();
        writer.commit();
    }
    probe::pump_until([&] { return count() == 1; }, "Admission did not commit after release");
    const auto observed = tick();
    post();
    std::vector<LONGLONG> ack;
    probe::pump_until([&] { ack = responses(receipt); return ack.size() == sent.size(); }, "Heartbeat did not drain");
    require(owner && GetClipboardOwner() == owner && GetClipboardSequenceNumber() == seq &&
            !(GetWindowLongPtrW(host.workspace, GWL_STYLE) & WS_VISIBLE), "Admission changed clipboard or visibility");
    persistence::Statement row(observer.get(), "SELECT id,snapshot_id FROM entries WHERE rope_id='main'");
    require(row.step(), "Capture missed its bound rope");
    const auto id = row.text(0), snapshot = row.text(1);
    require(!row.step(), "Duplicate admission");
    host.exit();
    HistoryStore reopened(profile);
    const auto entries = reopened.hanging("main");
    const auto saved = reopened.load(id);
    const auto expected = sample::formats(false);
    require(entries.size() == 1 && entries.front().id == id && entries.front().snapshot_id == snapshot &&
            std::any_of(saved.snapshot.formats.begin(), saved.snapshot.formats.end(), [&](const auto& f) {
                return f.standard_id == CF_UNICODETEXT && f.bytes == expected.front().bytes;
            }) && std::any_of(saved.snapshot.formats.begin(), saved.snapshot.formats.end(), [&](const auto& f) {
                return f.name == "HTML Format" && f.bytes == expected.back().bytes;
            }), "Restart changed the admitted identity or formats");
    std::cout << "listener_ready_ms=" << ms(ready - launch) << " writer_hold_ms=" << ms(release - read_done)
              << " release_to_visible_ms=" << ms(observed - release) << " seq=" << seq
              << " entry=" << id << " snapshot=" << snapshot << " heartbeat_ms=";
    for (size_t i = 0; i < ack.size(); ++i) std::cout << (i ? "," : "") << ms(ack[i] - sent[i]);
    std::cout << '\n';
}

void profile_reopen(const wchar_t* exe, const wchar_t* producer, const fs::path& profile) {
    // This path also runs against the restored Production executable, without a probe report.
    domain::Id id, snapshot;
    {
        probe::Resident fresh(exe, profile, false);
        auto observer = history_fixture::observe(profile);
        persistence::Statement version(observer.get(), "PRAGMA user_version");
        require(version.step() && version.integer(0) == 11 && !version.step(), "Unexpected fresh profile version");
        probe::ExternalCopy source(producer, L"normal");
        probe::pump_until([&] {
            persistence::Statement row(observer.get(), "SELECT id,snapshot_id FROM entries WHERE rope_id='main'");
            if (!row.step()) return false;
            id = row.text(0);
            snapshot = row.text(1);
            return true;
        }, "Fresh capture did not commit");
        fresh.exit();
    }
    probe::ExternalCopy external(producer, L"second");
    const auto sequence = GetClipboardSequenceNumber();
    probe::Resident previous(exe, profile, false);
    require(GetClipboardSequenceNumber() == sequence, "Startup replaced the external clipboard");
    previous.exit();
    HistoryStore reader(profile);
    const auto entries = reader.hanging("main");
    require(GetClipboardSequenceNumber() == sequence && entries.size() == 1 && entries.front().id == id &&
            entries.front().snapshot_id == snapshot &&
            reader.load(id).snapshot.formats.front().bytes == sample::formats(false).front().bytes,
            "Reopen changed v11 content, identity or clipboard");
    std::cout << "fresh+v11-reopen entry=" << id << " snapshot=" << snapshot << " sequence=" << sequence << '\n';
}

void listener_with_locked_debris(const wchar_t* exe, const wchar_t* producer, const fs::path& profile) {
    { HistoryStore seed(profile); }
    const auto debris = profile / "staging" / (std::string(32, 'c') + ".part");
    require(std::ofstream(debris).put('x').good(), "Cannot seed locked debris");
    const auto handle = CreateFileW(debris.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Cannot lock debris");
    std::unique_ptr<void, decltype(&CloseHandle)> hold(handle, CloseHandle);
    profile_reopen(exe, producer, profile);
    require(fs::exists(debris), "A locked file disappeared");
    hold.reset();
    HistoryStore reopened(profile);
    require(!fs::exists(debris) && reopened.hanging("main").size() == 1, "Retry lost history or left debris");
}

void thumbnail_access(const wchar_t* exe, const wchar_t* producer, const fs::path& profile) {
    domain::Format text, blob;
    text.standard_id = CF_UNICODETEXT;
    text.bytes = {'A', 0, 0, 0};
    blob.name = "Thumbnail access fixture";
    blob.bytes.assign(18000, 0x5a);
    domain::Id original, external, captured;
    {
        HistoryStore seed(profile);
        original = seed.admit("main", {{text}});
        external = seed.admit("main", {{blob}});
    }
    const auto cache = profile / "thumbnails";
    fs::create_directories(cache);
    const auto orphan = cache / (std::string(64, 'b') + "-png.thumb");
    require(std::ofstream(orphan).put('x').good(), "Cannot seed orphan thumbnail");
    {
        history_fixture::DenyDirectoryAccess deny(cache, FILE_LIST_DIRECTORY);
        std::error_code ec;
        fs::directory_iterator enumeration(cache, ec);
        require(GetFileAttributesW(cache.c_str()) != INVALID_FILE_ATTRIBUTES &&
                ec.value() == ERROR_ACCESS_DENIED, "Fixture must deny enumeration and permit attributes");
        probe::Resident host(exe, profile, false);
        auto reader = history_fixture::observe(profile);
        probe::ExternalCopy source(producer, L"normal");
        probe::pump_until([&] {
            persistence::Statement row(reader.get(), "SELECT id FROM entries WHERE id<>? AND id<>?");
            row.bind(1, original);
            row.bind(2, external);
            if (!row.step()) return false;
            captured = row.text(0);
            return true;
        }, "Listener did not capture while thumbnails were inaccessible");
        host.exit();
        HistoryStore reopen(profile);
        require(reopen.startup_collection().deferred == 1 &&
                reopen.load(original).snapshot.formats.front().bytes == text.bytes &&
                reopen.load(external).snapshot.formats.front().bytes == blob.bytes &&
                reopen.load(captured).snapshot.formats.front().bytes == sample::formats(false).front().bytes,
                "Deferred enumeration damaged history or failed to report incomplete cleanup");
    }
    HistoryStore retry(profile);
    require(retry.startup_collection().removed == 1 && !retry.startup_collection().deferred &&
            !fs::exists(orphan) && retry.hanging("main").size() == 3, "Thumbnail cleanup retry failed");
    std::cout << "enumeration denied; listener ready; capture and two original entries reopened; orphan reclaimed\n";
}

void capture_failure_report(const wchar_t* exe, const wchar_t* producer, const fs::path& profile, bool busy) {
    { HistoryStore seed(profile); }
    const auto receipt = profile / "capture.txt";
    probe::Resident host(exe, profile, false, receipt, busy ? nullptr : L"capture-full", {}, true);
    auto observer = history_fixture::observe(profile, SQLITE_OPEN_READWRITE);
    {
        std::optional<persistence::Transaction> blocker;
        if (busy) blocker.emplace(observer.get());
        probe::ExternalCopy copy(producer, L"normal");
        probe::pump_until([&] {
            std::ifstream in(receipt);
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            return text.find("capture history-failed") != text.npos;
        }, "Missing terminal capture failure");
    }
    host.exit();
    const auto report = history_fixture::diagnostic_report(profile);
    require(report.find("Failures: 1\r\n") != report.npos &&
            report.find(busy ? "Capture history: stale=0 busy=1 storage=0\r\n" :
                               "Capture history: stale=0 busy=0 storage=1\r\n") != report.npos,
            "Storage failure did not reach the requested export exactly once");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int status{};
    try {
        history_fixture::Directory dir;
        if (std::wstring_view(argv[3]) == L"--profiles") profile_reopen(argv[1], argv[2], dir.path);
        else if (std::wstring_view(argv[3]) == L"--locked-debris") listener_with_locked_debris(argv[1], argv[2], dir.path);
        else if (std::wstring_view(argv[3]) == L"--thumbnail-access") thumbnail_access(argv[1], argv[2], dir.path);
        else if (std::wstring_view(argv[3]) == L"--diagnostics") {
            capture_failure_report(argv[1], argv[2], dir.path / "busy", true);
            capture_failure_report(argv[1], argv[2], dir.path / "full", false);
        }
        else if (std::wstring_view(argv[3]) == L"--heartbeat")
            for (unsigned i = 0; i < 3; ++i) heartbeat(argv[1], argv[2], dir.path / std::to_string(i));
        else status = 2;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; status = 1; }
    OleUninitialize();
    return status;
}
