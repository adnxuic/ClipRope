#include "../integration/resident.hpp"
#include "../integration/history_fixture.hpp"
#include "../clipboard_sources/sample.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/workspace_commands.hpp"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <fstream>
#include <thread>

namespace {
using namespace cliprope;
using probe::require;
namespace fs = std::filesystem;
struct Close { void operator()(void* h) const { CloseHandle(h); } };
using Handle = std::unique_ptr<void, Close>;

std::vector<DWORD> children(DWORD parent, bool workers_only = true) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    require(snapshot.get() != INVALID_HANDLE_VALUE, "Cannot inspect worker children");
    PROCESSENTRY32W entry{sizeof(entry)};
    std::vector<DWORD> result;
    require(Process32FirstW(snapshot.get(), &entry) != FALSE, "Cannot inspect process table");
    do {
        if (entry.th32ParentProcessID == parent && (!workers_only || _wcsicmp(entry.szExeFile, L"ClipRopeWorker.exe") == 0))
            result.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot.get(), &entry));
    return result;
}
Handle access(DWORD pid, DWORD rights) {
    Handle h(OpenProcess(rights, FALSE, pid));
    require(h != nullptr, "Fixture could not open its worker");
    return h;
}
std::size_t private_bytes(HANDLE h) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    require(GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE,
            "Private bytes unavailable");
    return counters.PrivateUsage;
}
std::size_t memory(probe::Resident& resident, std::size_t expected_workers) {
    const auto pids = children(resident.pid());
    require(pids.size() == expected_workers, "Unexpected number of live workers");
    auto total = private_bytes(resident.handle());
    require(children(resident.pid(), false) == pids, "Unexpected helper would be excluded from resident total");
    for (auto pid : pids) {
        require(children(pid, false).empty(), "Unexpected worker child would be excluded from resident total");
        total += private_bytes(access(pid, PROCESS_QUERY_LIMITED_INFORMATION).get());
    }
    return total;
}
void reading(probe::ExternalCopy& copy) {
    probe::pump_until([&] { return WaitForSingleObject(copy.reading, 0) == WAIT_OBJECT_0; }, "Producer never received GetData");
}
void responsive(probe::Resident& host) {
    DWORD_PTR answer{};
    require(SendMessageTimeoutW(host.listener(), WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 250, &answer) != 0,
            "Main listener blocked during foreign read");
}
struct Reader {
    sqlite3* db{};
    explicit Reader(const fs::path& profile) {
        const auto path = (profile / "history.db").u8string();
        require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
                "Could not observe disposable store");
    }
    ~Reader() { sqlite3_close(db); }
    int count(const char* query) const {
        persistence::Statement row(db, query);
        require(row.step(), "Could not count disposable records");
        return static_cast<int>(row.integer(0));
    }
    int entries() const { return count("SELECT count(*) FROM entries"); }
};
void no_staging(const fs::path& profile) {
    require(fs::is_empty(profile / "staging"), "Blocked worker left staged data");
}

void raw_loop(const wchar_t* host_exe, bool contend) {
    history_fixture::Directory dir;
    const auto receipt = dir.path / "capture-test.txt";
    probe::Resident host(host_exe, dir.path, false, receipt);
    Reader db(dir.path);
    HWND owner = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    require(owner, "Native loop source unavailable");
    struct Clean {
        HWND source;
        ~Clean() {
            if (GetClipboardOwner() == source && OpenClipboard(source)) {
                if (GetClipboardOwner() == source) EmptyClipboard();
                CloseClipboard();
            }
            DestroyWindow(source);
        }
    } clean{owner};
    std::jthread reader;
    if (contend) reader = std::jthread([](std::stop_token stop) {
        HWND window = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
        while (!stop.stop_requested()) {
            if (OpenClipboard(window)) {
                Sleep(2);
                CloseClipboard();
            }
            Sleep(1);
        }
        DestroyWindow(window);
    });
    auto head = [&]() -> std::string {
        persistence::Statement q(db.db, "SELECT id FROM entries WHERE rope_id='main' ORDER BY sort_key,id LIMIT 1");
        return q.step() ? q.text(0) : "";
    };
    for (unsigned n = 1; n <= 500; ++n) {
        const auto before = head();
        const auto text = L"ClipRope synthetic " + std::to_wstring(n);
        const auto size = (text.size() + 1) * sizeof(wchar_t);
        auto data = GlobalAlloc(GMEM_MOVEABLE, size);
        auto p = GlobalLock(data);
        require(p, "Native loop allocation failed");
        std::memcpy(p, text.c_str(), size);
        GlobalUnlock(data);
        probe::pump_until([&] { return OpenClipboard(owner) != FALSE; }, "Native loop source could not open clipboard");
        const bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data);
        CloseClipboard();
        require(ok, "Native loop publication failed");
        const auto seq = GetClipboardSequenceNumber();
        const auto until = GetTickCount64() + 4000;
        while (head() == before && GetTickCount64() < until) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(1);
        }
        if (head() == before) {
            std::cout << "raw-copy=" << n << " sequence=" << seq << " current=" << GetClipboardSequenceNumber()
                      << " same-owner=" << (GetClipboardOwner() == owner) << std::endl;
            std::ifstream log(receipt);
            std::string line;
            while (std::getline(log, line)) if (line.find("seq=" + std::to_string(seq)) != line.npos) std::cerr << line << '\n';
            throw std::runtime_error("Native Unicode loop lost a stable copy");
        }
    }
    host.exit();
    std::cout << "same-owner native CF_UNICODETEXT: 500/500 versions admitted; competing-reader=" << contend << '\n';
}

void stable_contention(const wchar_t* host_exe, const wchar_t* source_exe) {
    history_fixture::Directory dir;
    probe::Resident host(host_exe, dir.path, false);
    probe::ExternalCopy locker(source_exe, L"reader-250");
    Reader db(dir.path);
    const auto text = sample::formats(false).front();
    HWND owner = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    require(owner != nullptr, "Raw text source window unavailable");
    struct Clean {
        HWND window;
        ~Clean() {
            if (GetClipboardOwner() == window && OpenClipboard(window)) {
                if (GetClipboardOwner() == window) EmptyClipboard();
                CloseClipboard();
            }
            DestroyWindow(window);
        }
    } clean{owner};
    HGLOBAL bytes = GlobalAlloc(GMEM_MOVEABLE, text.bytes.size());
    auto data = GlobalLock(bytes);
    require(data != nullptr, "Fixture allocation failed");
    std::memcpy(data, text.bytes.data(), text.bytes.size());
    GlobalUnlock(bytes);
    require(OpenClipboard(owner) != FALSE, "Raw clipboard unavailable");
    const bool written = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, bytes);
    SetEvent(locker.proceed);
    CloseClipboard();
    require(written, "Fixture raw copy failed");
    const auto sequence = GetClipboardSequenceNumber();
    const auto start = GetTickCount64();
    reading(locker);
    auto other = GetOpenClipboardWindow();
    DWORD locker_pid{};
    GetWindowThreadProcessId(other, &locker_pid);
    require(other && locker_pid != GetCurrentProcessId() && locker_pid != host.pid(), "Reader was not external");
    probe::pump_until([&] { return GetOpenClipboardWindow() != other; }, "External reader kept clipboard open");
    const auto held = GetTickCount64() - start;
    require(held >= 210 && GetClipboardOwner() == owner && GetClipboardSequenceNumber() == sequence,
            "Sequence or ownership changed during read contention");
    while (!db.entries() && GetTickCount64() - start < 3200) Sleep(5);
    const auto admitted = db.entries();
    std::cout << "raw-unicode=yes stable-version=" << sequence << " external-lock-ms=" << held
              << " rows=" << admitted << " elapsed-ms=" << GetTickCount64() - start << std::endl;
    require(admitted == 1 && GetClipboardSequenceNumber() == sequence && GetClipboardOwner() == owner,
            "Stable eligible Unicode copy was lost during reader contention");
    host.exit();
    persistence::HistoryStore reopened(dir.path);
    const auto entries = reopened.hanging(reopened.active_rope());
    require(entries.size() == 1, "Contention retries duplicated the copy");
    const auto loaded = reopened.load(entries.front().id);
    require(std::any_of(loaded.snapshot.formats.begin(), loaded.snapshot.formats.end(), [&](const auto& row) {
        return row.standard_id == CF_UNICODETEXT && row.outcome == "saved" && row.bytes == text.bytes;
    }), "Stable copy lost Unicode bytes");
}

void qualification(const wchar_t* exe, const wchar_t* producer) {
    history_fixture::Directory dir;
    {
        probe::Resident host(exe, dir.path, false);
        Reader reader(dir.path);
        probe::ExternalCopy first(producer, L"blocked");
        reading(first);
        const auto old_pid = children(host.pid());
        require(old_pid.size() == 1, "First ticket did not own one child");
        probe::ExternalCopy second(producer, L"blocked");
        reading(second);
        require(children(host.pid()).size() == 2, "Second hung source blocked the dispatcher");
        probe::ExternalCopy third(producer, L"paused");
        Sleep(75);
        responsive(host);
        probe::ExternalCopy latest(producer, L"second");
        Sleep(75);
        responsive(host);
        require(children(host.pid()).size() == 2 && WaitForSingleObject(third.reading, 0) == WAIT_TIMEOUT,
                "Queue exceeded two active workers");
        require(reader.entries() == 0 && reader.count("SELECT count(*) FROM blobs") == 0, "Pending capture wrote history");
        no_staging(dir.path);
        Handle crashed = access(old_pid.front(), PROCESS_TERMINATE | SYNCHRONIZE);
        require(TerminateProcess(crashed.get(), 77) && WaitForSingleObject(crashed.get(), 1500) == WAIT_OBJECT_0,
                "Could not kill exact first worker");
        probe::pump_until([&] { return reader.entries() == 1; }, "Latest eligible ticket never committed after worker death");
        require(WaitForSingleObject(third.reading, 0) == WAIT_TIMEOUT, "Superseded waiting ticket ran");
        probe::pump_until([&] { return children(host.pid()).empty(); }, "Second blocked worker did not expire");
        SetEvent(first.proceed);
        SetEvent(second.proceed);
        probe::ExternalCopy crashed_current(producer, L"blocked");
        reading(crashed_current);
        auto current = children(host.pid());
        require(current.size() == 1, "Crash case was not isolated");
        auto victim = access(current.front(), PROCESS_TERMINATE | SYNCHRONIZE);
        require(TerminateProcess(victim.get(), 78) && WaitForSingleObject(victim.get(), 1500) == WAIT_OBJECT_0,
                "Failed to terminate current worker");
        SetEvent(crashed_current.proceed);
        responsive(host);
        // Keep this source and its sequence current until the three-second deadline.
        probe::ExternalCopy expiring(producer, L"blocked");
        reading(expiring);
        const auto begin = GetTickCount64();
        probe::pump_until([&] { responsive(host); return children(host.pid()).empty(); }, "Active job exceeded its deadline");
        const auto elapsed = GetTickCount64() - begin;
        require(elapsed > 2000 && elapsed < 4000 && reader.entries() == 1, "Expiry timing or stale admission failed");
        SetEvent(expiring.proceed);
        probe::ExternalCopy recovered(producer, L"normal");
        probe::pump_until([&] { return reader.entries() == 2 && children(host.pid()).empty(); }, "Recovery did not admit once");
        responsive(host);
        no_staging(dir.path);
        host.exit();
        std::cout << "queue=2+latest dropped-payload-reads=0 killed=2 current-expiry-ms=" << elapsed
                  << " expired-admissions=0 late-admissions=0 recovered-once=1 live-workers=0 staging=0\n";
    }
    persistence::HistoryStore check(dir.path);
    const auto entries = check.hanging(check.active_rope());
    require(entries.size() == 2, "Wrong durable admission count after restart");
    for (size_t i = 0; i < 2; ++i) {
        const auto data = check.load(entries[i].id);
        auto bytes = sample::formats(i == 1).front().bytes;
        const auto& formats = data.snapshot.formats;
        require(std::any_of(formats.begin(), formats.end(), [&](const auto& value) {
                    return value.standard_id == CF_UNICODETEXT && value.bytes == bytes;
                }), "Wrong durable bytes after recovery");
    }
}

void compare(const wchar_t* exe, const wchar_t* producer, bool isolated) {
    using Clock = std::chrono::steady_clock;
    history_fixture::Directory dir;
    probe::Resident host(exe, dir.path, false);
    const auto idle = memory(host, 0);
    probe::ExternalCopy held(producer, L"blocked");
    reading(held);
    const auto blocked = memory(host, isolated ? 1 : 0);
    const auto ping = Clock::now();
    responsive(host);
    const auto response = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - ping).count();
    const auto begin = Clock::now();
    require(PostMessageW(host.listener(), WM_CLOSE, 0, 0) != FALSE, "Cannot request shutdown");
    const auto early = WaitForSingleObject(host.handle(), 750);
    require(early == (isolated ? WAIT_OBJECT_0 : WAIT_TIMEOUT), "Unexpected recovery while source still blocked");
    SetEvent(held.proceed);
    require(WaitForSingleObject(host.handle(), 3000) == WAIT_OBJECT_0, "Shutdown did not finish after release");
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
    DWORD code{};
    require(GetExitCodeProcess(host.handle(), &code) && code == 0, "Shutdown failed");
    std::cout << "model=" << (isolated ? "isolated" : "legacy-sta") << " idle-private-bytes=" << idle
              << " blocked-private-bytes=" << blocked << " ping-us=" << response << " shutdown-us=" << elapsed << '\n';
}

PTP_WAIT WINAPI unavailable_wait(PTP_WAIT_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

void wait_for_admission(probe::Resident& host) {
    probe::pump_until([&] {
        DWORD_PTR active{};
        require(SendMessageTimeoutW(host.listener(), win::performance_query, WPARAM(win::PerformanceCounter::capture_writing),
                                    0, SMTO_ABORTIFHUNG, 200, &active), "Input waited for admission");
        return active != 0;
    }, "Capture never accepted the writer request");
}

bool saved_unicode(persistence::HistoryStore& store, const domain::Id& id, bool second) {
    const auto saved = store.load(id);
    const auto bytes = sample::formats(second).front().bytes;
    return std::any_of(saved.snapshot.formats.begin(), saved.snapshot.formats.end(), [&](const auto& format) {
        return format.standard_id == CF_UNICODETEXT && format.outcome == "saved" && format.bytes == bytes;
    });
}

void close_admission(const wchar_t* exe, const wchar_t* producer, bool fail) {
    history_fixture::Directory dir;
    domain::Id tail, snapshot;
    {
        persistence::HistoryStore store(dir.path);
        store.set_capacity("main", 1);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(true).front().bytes;
        tail = store.admit("main", {{text}});
        snapshot = store.entry(tail).snapshot_id;
    }
    const auto report = dir.path / "close.txt";
    probe::Resident host(exe, dir.path, false, report, fail ? L"capture-full" : nullptr);
    sqlite3* raw{};
    const auto path = (dir.path / "history.db").u8string();
    require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not delay capture exit fixture");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    persistence::Transaction hold(raw);
    probe::ExternalCopy copy(producer, L"normal");
    wait_for_admission(host);
    DWORD_PTR ignored{};
    require(SendMessageTimeoutW(host.listener(), WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 300, &ignored) &&
            WaitForSingleObject(host.handle(), 0) == WAIT_TIMEOUT, "Close waited on SQL or abandoned the accepted copy");
    hold.commit();
    probe::pump_until([&] { return WaitForSingleObject(host.handle(), 0) == WAIT_OBJECT_0; }, "Close did not drain the accepted writer");
    DWORD code{};
    require(GetExitCodeProcess(host.handle(), &code) && code == (fail ? 1u : 0u), "Close lost the admission disposition");
    std::ifstream input(report);
    unsigned terminal{};
    std::string line;
    while (std::getline(input, line))
        terminal += line.starts_with(fail ? "capture history-failed " : "capture captured ");
    require(terminal == 1, "Close lost or duplicated the terminal receipt");
    probe::Resident restarted(exe, dir.path, false);
    restarted.exit();
    persistence::HistoryStore reopened(dir.path);
    const auto entries = reopened.hanging("main");
    require(entries.size() == 1 && (entries[0].id == tail) == fail && (entries[0].snapshot_id == snapshot) == fail &&
            saved_unicode(reopened, entries[0].id, fail),
            "Close/restart lost the accepted write or evicted the previous head on failure");
    std::cout << "close-while-writing fail=" << fail << " exit=" << code << " previous=" << tail
              << " reopened=" << entries.front().id << " terminal=1\n";
}

void admission_order(const wchar_t* exe, const wchar_t* producer, bool supersede, bool reject_switch = false) {
    history_fixture::Directory tmp;
    domain::Id old_tail, seed_entry, other, other_entry;
    {
        persistence::HistoryStore seed(tmp.path);
        seed.set_capacity("main", 2);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(true).front().bytes;
        old_tail = seed.admit("main", {{text}});
        seed_entry = seed.admit("main", {{text}});
        other = seed.create_rope("Other", 2);
        other_entry = seed.admit(other, {{text}});
    }
    const auto report = tmp.path / "order.txt";
    probe::Resident host(exe, tmp.path, true, report);
    const auto database = (tmp.path / "history.db").u8string();
    sqlite3* raw{};
    require(sqlite3_open_v2(reinterpret_cast<const char*>(database.c_str()), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not delay synthetic capture");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    persistence::Transaction lock(raw);
    if (reject_switch)
        persistence::exec(raw, "CREATE TRIGGER reject_switch BEFORE UPDATE OF active_rope_id ON app_settings "
                               "BEGIN SELECT RAISE(ABORT,'rejected switch'); END");
    probe::ExternalCopy x(producer, L"normal");
    wait_for_admission(host);
    auto seq = GetClipboardSequenceNumber();
    auto owner = GetClipboardOwner();
    // Bind the actual card and paste preference while the accepted write is held.
    DWORD_PTR result{};
    const auto center = SendMessageW(host.listener(), win::performance_query, WPARAM(win::PerformanceCounter::first_card_point), 0);
    POINT point{short(LOWORD(center)), short(HIWORD(center))};
    require(center && ScreenToClient(host.workspace, &point), "No committed card geometry for the press");
    for (const UINT message : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP})
        require(SendMessageTimeoutW(host.workspace, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON,
                                    MAKELPARAM(point.x, point.y), SMTO_ABORTIFHUNG, 700, &result),
                "Bound card gesture waited on admission");
    if (supersede)
        require(SendMessageTimeoutW(host.workspace, WM_COMMAND, WPARAM(win::WorkspaceCommand::next), 0,
                                    SMTO_ABORTIFHUNG, 200, &result), "Collector switch blocked the dispatcher");
    require(GetClipboardSequenceNumber() == seq && GetClipboardOwner() == owner, "Manual effect escaped before commit");
    std::unique_ptr<probe::ExternalCopy> y;
    if (supersede) {
        y = std::make_unique<probe::ExternalCopy>(producer, L"second");
        seq = GetClipboardSequenceNumber();
        owner = GetClipboardOwner();
        require(SendMessageTimeoutW(host.listener(), WM_CLIPBOARDUPDATE, 0, 0, SMTO_ABORTIFHUNG, 200, &result),
                "New notification blocked during admission");
    }
    lock.commit();
    const auto contains = [&](std::string_view needle) {
        std::ifstream input(report);
        std::string line;
        while (std::getline(input, line)) if (line.find(needle) != line.npos) return true;
        return false;
    };
    probe::pump_until([&] {
        return contains(supersede ? "invoke superseded " + seed_entry : "invoke double-click " + seed_entry + " published ");
    }, "Deferred manual publication had the wrong disposition");
    if (supersede) {
        probe::pump_until([&] {
            if (reject_switch) {
                persistence::Statement q(raw, "SELECT count(*) FROM entries WHERE id=?");
                q.bind(1, seed_entry);
                return q.step() && q.integer(0) == 0; // Only Y on main can now evict the earlier head.
            }
            persistence::Statement q(raw, "SELECT count(*) FROM entries WHERE rope_id=?");
            q.bind(1, other);
            return q.step() && q.integer(0) == 2;
        }, "External replacement did not follow the switch's actual disposition");
        require(GetClipboardSequenceNumber() == seq && GetClipboardOwner() == owner &&
                !contains("follow-head published") && !contains("invoke double-click " + seed_entry + " published") &&
                (GetWindowLongPtrW(host.workspace, GWL_STYLE) & WS_VISIBLE),
                "Stale command replaced the newer clipboard");
    } else {
        require(GetClipboardSequenceNumber() != seq && GetClipboardOwner() != owner &&
                !(GetWindowLongPtrW(host.workspace, GWL_STYLE) & WS_VISIBLE), "Manual recall did not publish and collapse after commit");
    }
    const auto peak = SendMessageW(host.listener(), win::performance_query, WPARAM(win::PerformanceCounter::admission_peak_bytes), 0);
    host.exit();
    persistence::HistoryStore reopened(tmp.path);
    const auto a = reopened.hanging("main"), b = reopened.hanging(other);
    const auto x_index = supersede && !reject_switch ? 0u : 1u;
    require(a.size() == 2 && a[x_index].id != old_tail && a[x_index].id != seed_entry &&
            (a[1 - x_index].id == seed_entry) != reject_switch && b.size() == (supersede && !reject_switch ? 2u : 1u) &&
            b.back().id == other_entry &&
            reopened.active_rope() == (supersede && !reject_switch ? other : "main") && peak > 0 && peak <= LRESULT(snapshot_byte_limit) &&
            saved_unicode(reopened, a[x_index].id, false),
            "Restart changed command order, victim, collector or queue bound");
    if (supersede) {
        const auto& newest = reject_switch ? a.front() : b.front();
        require(saved_unicode(reopened, newest.id, true) &&
                contains("invoke double-click " + seed_entry + " failed skipped skipped"), "Wrong replacement bytes or native dispatch");
    }
    if (!supersede) require(probe::clear_private_sample(sample::text(true)), "Could not retire private head");
    std::cout << "supersede=" << supersede << " switch-rejected=" << reject_switch << " admitted=" << a[x_index].id << " evicted=" << old_tail
              << " confirmed-card=" << seed_entry << " peak-pending-bytes=" << peak << '\n';
}

void supervision(const wchar_t* exe, const wchar_t* producer) {
    history_fixture::Directory tmp;
    {
        persistence::HistoryStore store(tmp.path / "withheld");
        win::HistoryClipboard clipboard;
        win::HistoryCapture capture(store, clipboard);
        probe::ExternalCopy held(producer, L"blocked");
        require(capture.observe().status == "pending", "Blocked ticket was not accepted for extraction");
        reading(held);
        const auto workers = children(GetCurrentProcessId());
        require(workers.size() == 1, "Missing supervised worker");
        auto child = access(workers.front(), SYNCHRONIZE);
        // Neither pump the host STA nor call poll() until the child exits.
        require(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0, "Deadline depended on UI dispatch");
        auto expired = capture.poll();
        require(expired.size() == 1 && expired.front().status == "expired" &&
                capture.poll().empty() && !capture.busy() && store.hanging("main").empty(),
                "Withheld result admitted or completed twice");
        SetEvent(held.proceed);
    }
    {
        persistence::HistoryStore store(tmp.path / "registration");
        win::HistoryClipboard clipboard;
        win::HistoryCapture capture(store, clipboard, unavailable_wait);
        probe::ExternalCopy external(producer, L"normal");
        capture.observe();
        const auto failed = capture.poll();
        require(failed.size() == 1 && failed.front().status == "worker-unavailable" &&
                failed.front().detail.find("win32=8") != std::string::npos && !capture.busy(),
                "Registration failure left a pending ticket");
        probe::pump_until([&] { return children(GetCurrentProcessId()).empty(); }, "Failed registration left a child");
    }
    const auto profile = tmp.path / "timer";
    fs::create_directory(profile);
    const auto report = profile / "events.txt";
    probe::Resident resident(exe, profile, false, report, L"capture-timer");
    probe::ExternalCopy external(producer, L"second");
    Reader db(profile);
    probe::pump_until([&] { return db.entries() == 1; }, "Unavailable USER timer stranded the latest ticket");
    resident.exit();
    std::ifstream events(report);
    const std::string log((std::istreambuf_iterator<char>(events)), {});
    require(log.find("capture timer unavailable") != log.npos, "Timer failure seam was not exercised");
    std::cout << "withheld-STA=child-exited-before-poll expiry=once wait-registration-failure=reaped "
                 "timer-failure=one-admission\n";
}
}
int wmain(int argc, wchar_t** argv) {
    if ((argc != 3 && argc != 4) || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int status = 0;
    try {
        if (argc == 3 && std::wstring_view(argv[2]).starts_with(L"--raw-loop"))
            raw_loop(argv[1], std::wstring_view(argv[2]) == L"--raw-loop-contention");
        else if (argc == 4 && std::wstring_view(argv[3]) == L"--close-admission") {
            close_admission(argv[1], argv[2], false);
            close_admission(argv[1], argv[2], true);
        } else if (argc == 4 && std::wstring_view(argv[3]) == L"--admission-order") {
            admission_order(argv[1], argv[2], false);
            admission_order(argv[1], argv[2], true);
            admission_order(argv[1], argv[2], true, true);
        } else if (argc == 4 && std::wstring_view(argv[3]) == L"--supervision") supervision(argv[1], argv[2]);
        else if (argc == 4 && std::wstring_view(argv[3]) == L"--contention") stable_contention(argv[1], argv[2]);
        else if (argc == 3) { qualification(argv[1], argv[2]); compare(argv[1], argv[2], true); }
        else { compare(argv[3], argv[2], false); compare(argv[1], argv[2], true); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    OleUninitialize();
    return status;
}
